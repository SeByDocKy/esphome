#include <gtest/gtest.h>

#include <array>
#include <deque>
#include <vector>

#include "esphome/components/weikai/weikai.h"

namespace esphome::weikai::testing {

/// State of a fake WeiKai channel: the bytes of the receive FIFO and the bytes written to the transmit FIFO.
struct FakeChip {
  std::deque<uint8_t> rx_fifo;
  std::vector<uint8_t> tx_written;
  int fsr_reads{0};
  int gifr_reads{0};  // only counted in chip 0
};

class FakeRegister : public WeikaiRegister {
 public:
  FakeRegister(WeikaiComponent *comp, FakeChip *chips) : WeikaiRegister(comp, 0, 0), chips_(chips) {}

  uint8_t read_reg() const override {
    FakeChip *chip_ = &this->chips_[this->channel_];
    switch (this->register_) {
      case WKREG_GIFR: {
        uint8_t flags = 0;
        for (int i = 0; i < 4; i++)
          if (!this->chips_[i].rx_fifo.empty())
            flags |= 1 << i;
        this->chips_[0].gifr_reads++;
        return flags;
      }
      case WKREG_FSR:
        chip_->fsr_reads++;
        return chip_->rx_fifo.empty() ? 0 : FSR_RFDAT;
      case WKREG_RFCNT:
        return static_cast<uint8_t>(chip_->rx_fifo.size());  // 256 bytes read as 0
      case WKREG_LCR:
        return this->lcr_;
      default:
        return 0;
    }
  }
  void write_reg(uint8_t value) override {
    if (this->register_ == WKREG_LCR)
      this->lcr_ = value;
  }
  void read_fifo(uint8_t *data, size_t length) const override {
    FakeChip *chip_ = &this->chips_[this->channel_];
    for (size_t i = 0; i < length; i++) {
      data[i] = chip_->rx_fifo.front();
      chip_->rx_fifo.pop_front();
    }
  }
  void write_fifo(uint8_t *data, size_t length) override {
    FakeChip *chip_ = &this->chips_[this->channel_];
    chip_->tx_written.insert(chip_->tx_written.end(), data, data + length);
  }

 protected:
  FakeChip *chips_;
  uint8_t lcr_{0};  // read back by the channel test done in setup_channel()
};

class FakeComponent : public WeikaiComponent {
 public:
  FakeComponent() : chip(this->chips[0]), reg_(this, this->chips.data()) { this->set_crystal(14745600); }
  WeikaiRegister &reg(uint8_t reg, uint8_t channel) override {
    this->reg_.register_ = reg;
    this->reg_.channel_ = channel;
    return this->reg_;
  }
  std::array<FakeChip, 4> chips;
  FakeChip &chip;  // channel 0

 protected:
  FakeRegister reg_;
};

class WeikaiChannelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    this->channel_.set_parent(&this->comp_);
    this->channel_.set_channel(0);
    this->channel_.set_channel_name("test");
    this->channel_.set_baud_rate(115200);
    this->channel_.setup_channel();
  }
  FakeComponent comp_;
  WeikaiChannel channel_;
};

TEST_F(WeikaiChannelTest, ReceivesFullFifoInOrder) {
  for (int i = 0; i < 256; i++)
    this->comp_.chip.rx_fifo.push_back(i);
  EXPECT_EQ(this->channel_.available(), 256u);  // RFCNT reads 0 for 256 bytes
  std::vector<uint8_t> data(256);
  EXPECT_TRUE(this->channel_.read_array(data.data(), data.size()));
  for (int i = 0; i < 256; i++)
    EXPECT_EQ(data[i], i);
  EXPECT_EQ(this->channel_.available(), 0u);
}

TEST_F(WeikaiChannelTest, IdleChannelCostsOneStatusRead) {
  this->comp_.chip.fsr_reads = 0;
  EXPECT_EQ(this->channel_.available(), 0u);
  EXPECT_EQ(this->comp_.chip.fsr_reads, 1);
}

TEST_F(WeikaiChannelTest, FullBufferLeavesTheRestInTheFifo) {
  for (int i = 0; i < 256; i++)
    this->comp_.chip.rx_fifo.push_back(i);
  EXPECT_EQ(this->channel_.available(), 256u);
  for (int i = 0; i < 10; i++)  // arrives while the ring buffer is full
    this->comp_.chip.rx_fifo.push_back(200 + i);
  std::vector<uint8_t> data(100);
  EXPECT_TRUE(this->channel_.read_array(data.data(), data.size()));
  EXPECT_EQ(this->channel_.available(), 156u);
  EXPECT_EQ(this->comp_.chip.rx_fifo.size(), 10u);  // not read yet: the buffer was not empty
  std::vector<uint8_t> rest(156);
  EXPECT_TRUE(this->channel_.read_array(rest.data(), rest.size()));
  EXPECT_EQ(rest.front(), 100);
  EXPECT_EQ(this->channel_.available(), 10u);  // the buffer is empty: the FIFO is read again
  uint8_t first{};
  EXPECT_TRUE(this->channel_.peek_byte(&first));
  EXPECT_EQ(first, 200);
}

TEST_F(WeikaiChannelTest, ReadingMoreThanAvailableFails) {
  this->comp_.chip.rx_fifo.push_back(1);
  EXPECT_EQ(this->channel_.available(), 1u);
  uint8_t data[4] = {};
  EXPECT_FALSE(this->channel_.read_array(data, sizeof(data)));
  EXPECT_EQ(data[0], 1);
}

TEST_F(WeikaiChannelTest, LongWriteIsSentInChunks) {
  std::vector<uint8_t> out(300);
  for (size_t i = 0; i < out.size(); i++)
    out[i] = i;
  this->channel_.write_array(out.data(), out.size());
  EXPECT_EQ(this->comp_.chip.tx_written, out);
}

TEST_F(WeikaiChannelTest, BaudRateTooHighIsReduced) {
  this->channel_.set_baud_rate(10000000);
  this->channel_.setup_channel();
  EXPECT_EQ(this->channel_.get_baud_rate(), 14745600u / 16);
}

TEST_F(WeikaiChannelTest, BaudRateTooLowReportsTheRateReallyUsed) {
  this->channel_.set_baud_rate(1);
  this->channel_.setup_channel();
  EXPECT_EQ(this->channel_.get_baud_rate(), 14u);  // 14745600 / (16 * 65536.9)
}

#ifdef USE_WEIKAI_GIFR
class GifrPollTest : public ::testing::Test {
 protected:
  void SetUp() override {
    this->comp_.set_gifr_poll(true);
    for (uint8_t i = 0; i < 4; i++) {
      this->channels_[i].set_parent(&this->comp_);
      this->channels_[i].set_channel(i);
      this->channels_[i].set_channel_name("test");
      this->channels_[i].set_baud_rate(115200);
      this->channels_[i].setup_channel();
    }
    for (auto &chip : this->comp_.chips)
      chip.fsr_reads = 0;
    this->comp_.chips[0].gifr_reads = 0;
  }
  FakeComponent comp_;
  std::array<WeikaiChannel, 4> channels_;
};

TEST_F(GifrPollTest, IdleHubCostsOneRead) {
  this->comp_.loop();
  EXPECT_EQ(this->comp_.chips[0].gifr_reads, 1);
  for (auto &chip : this->comp_.chips)
    EXPECT_EQ(chip.fsr_reads, 0);
}

TEST_F(GifrPollTest, OnlyTheActiveChannelIsRead) {
  this->comp_.chips[2].rx_fifo.push_back(0x55);
  this->comp_.loop();
  EXPECT_EQ(this->comp_.chips[0].gifr_reads, 1);
  EXPECT_EQ(this->comp_.chips[0].fsr_reads, 0);
  EXPECT_EQ(this->comp_.chips[1].fsr_reads, 0);
  EXPECT_EQ(this->comp_.chips[2].fsr_reads, 1);
  EXPECT_EQ(this->comp_.chips[3].fsr_reads, 0);
  uint8_t data{};
  EXPECT_TRUE(this->channels_[2].read_array(&data, 1));
  EXPECT_EQ(data, 0x55);
}
#endif

}  // namespace esphome::weikai::testing
