#pragma once
#include <opencv2/core.hpp>
#include <vector>

namespace affinecodec {
// Source includes actual samples and even/odd counterpart copies only.
// Neither source nor receipt mask is modified. Matches the browser NN/kernel.
cv::Mat fillRestartGaps(const cv::Mat& source,
                        const std::vector<unsigned char>& received, bool smooth);
}
