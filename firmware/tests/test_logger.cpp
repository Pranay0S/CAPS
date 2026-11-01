#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "fw/logger.h"

namespace {

struct Capture {
    std::vector<std::string> lines;
    static void Sink(void *ctx, const char *line) { static_cast<Capture *>(ctx)->lines.emplace_back(line); }
};

class LoggerTest : public ::testing::Test {
protected:
    void SetUp() override { logger_init(&lg, &Capture::Sink, &cap, LOG_INFO); }
    logger_t lg;
    Capture  cap;
};

TEST_F(LoggerTest, FormatsEventAsCsvRecord)
{
    ASSERT_TRUE(logger_event(&lg, 1500, LOG_WARN, "PUMP", "rate %d", 42));
    EXPECT_EQ(logger_flush(&lg), 1u);
    ASSERT_EQ(cap.lines.size(), 1u);
    EXPECT_EQ(cap.lines[0], "LOG,1500,WARN,PUMP,rate 42");
}

TEST_F(LoggerTest, CommasInMessageCannotBreakTheCsvRow)
{
    logger_event(&lg, 0, LOG_INFO, "A,B", "x,y\nz");
    logger_flush(&lg);
    EXPECT_EQ(cap.lines[0], "LOG,0,INFO,A;B,x;y z");
}

TEST_F(LoggerTest, DropsEventsBelowMinimumLevel)
{
    EXPECT_FALSE(logger_event(&lg, 0, LOG_DEBUG, "M", "hidden"));
    logger_set_level(&lg, LOG_DEBUG);
    EXPECT_TRUE(logger_event(&lg, 0, LOG_DEBUG, "M", "shown"));
    logger_flush(&lg);
    ASSERT_EQ(cap.lines.size(), 1u);
    EXPECT_NE(cap.lines[0].find("shown"), std::string::npos);
}

TEST_F(LoggerTest, FlushPreservesFifoOrderAndRawRecords)
{
    logger_raw(&lg, "TLM,1");
    logger_event(&lg, 2, LOG_INFO, "M", "two");
    logger_raw(&lg, "ACK,3,x,y");
    logger_flush(&lg);
    ASSERT_EQ(cap.lines.size(), 3u);
    EXPECT_EQ(cap.lines[0], "TLM,1");
    EXPECT_EQ(cap.lines[2], "ACK,3,x,y");
}

TEST_F(LoggerTest, OverflowIsCountedAndReportedOnFlush)
{
    for (int i = 0; i < LOG_RING_LINES; ++i)
        ASSERT_TRUE(logger_raw(&lg, "x"));
    EXPECT_FALSE(logger_raw(&lg, "lost"));
    EXPECT_FALSE(logger_event(&lg, 99, LOG_ERROR, "M", "lost too"));
    EXPECT_EQ(lg.dropped, 2u);

    EXPECT_EQ(logger_flush(&lg), static_cast<size_t>(LOG_RING_LINES) + 1);
    EXPECT_EQ(cap.lines.back(), "LOG,99,WARN,LOG,ring overflow: 2 lines dropped");
    EXPECT_EQ(lg.dropped, 0u);
}

TEST_F(LoggerTest, RingWrapsAround)
{
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < LOG_RING_LINES; ++i)
            logger_raw(&lg, std::to_string(round * 1000 + i).c_str());
        logger_flush(&lg);
    }
    ASSERT_EQ(cap.lines.size(), 3u * LOG_RING_LINES);
    EXPECT_EQ(cap.lines.back(), std::to_string(2000 + LOG_RING_LINES - 1));
}

TEST(LoggerLevel, ParsesNamesCaseInsensitively)
{
    log_level_t l;
    ASSERT_TRUE(logger_parse_level("warn", &l));
    EXPECT_EQ(l, LOG_WARN);
    EXPECT_FALSE(logger_parse_level("loud", &l));
}

} // namespace
