#include "udp_image_codec.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace affinecodec {
namespace {

constexpr std::uint32_t kMagic = 0x31434641u;
constexpr std::uint8_t kVersion = 2;
constexpr std::uint8_t kPacketKeyframeChunk = 1;
constexpr std::uint8_t kPacketPatch = 2;

constexpr int kFeatureGridX = 8;
constexpr int kFeatureGridY = 8;
constexpr int kFeaturesPerCell = 3;
constexpr int kMeshGridX = 4;
constexpr int kMeshGridY = 4;
const cv::Size kLkWindow(21, 21);
constexpr int kLkMaxLevel = 5;
constexpr float kLkForwardErrorMax = 35.0f;
constexpr float kLkBackwardErrorMax = 1.5f;
constexpr float kResidualMax = 10.0f;
constexpr std::size_t kCommonHeaderBytes = 20;
constexpr std::size_t kKeyframeChunkHeaderBytes = 40;
constexpr std::size_t kKeyframeChunkPayloadBytes = kMaxUdpPacketBytes - kKeyframeChunkHeaderBytes;
constexpr std::uint32_t kMaxAcceptedJpegBytes = 64u * 1024u * 1024u;
constexpr std::size_t kMaxPendingPatches = 64;

void appendU8(std::vector<u_char>& out, std::uint8_t v) { out.push_back(v); }
void appendU16(std::vector<u_char>& out, std::uint16_t v) {
    out.push_back(static_cast<u_char>(v & 0xffu));
    out.push_back(static_cast<u_char>((v >> 8) & 0xffu));
}
void appendU32(std::vector<u_char>& out, std::uint32_t v) {
    out.push_back(static_cast<u_char>(v & 0xffu));
    out.push_back(static_cast<u_char>((v >> 8) & 0xffu));
    out.push_back(static_cast<u_char>((v >> 16) & 0xffu));
    out.push_back(static_cast<u_char>((v >> 24) & 0xffu));
}
void appendFloat(std::vector<u_char>& out, float v) {
    std::uint32_t bits;
    static_assert(sizeof(bits) == sizeof(v), "32-bit float required");
    std::memcpy(&bits, &v, sizeof(bits));
    appendU32(out, bits);
}

bool readU8(const std::vector<u_char>& data, std::size_t& pos, std::uint8_t& v) {
    if (pos + 1 > data.size()) return false;
    v = data[pos++]; return true;
}
bool readU16(const std::vector<u_char>& data, std::size_t& pos, std::uint16_t& v) {
    if (pos + 2 > data.size()) return false;
    v = static_cast<std::uint16_t>(data[pos]) |
        (static_cast<std::uint16_t>(data[pos + 1]) << 8);
    pos += 2; return true;
}
bool readU32(const std::vector<u_char>& data, std::size_t& pos, std::uint32_t& v) {
    if (pos + 4 > data.size()) return false;
    v = static_cast<std::uint32_t>(data[pos]) |
        (static_cast<std::uint32_t>(data[pos + 1]) << 8) |
        (static_cast<std::uint32_t>(data[pos + 2]) << 16) |
        (static_cast<std::uint32_t>(data[pos + 3]) << 24);
    pos += 4; return true;
}
bool readFloat(const std::vector<u_char>& data, std::size_t& pos, float& v) {
    std::uint32_t bits;
    if (!readU32(data, pos, bits)) return false;
    std::memcpy(&v, &bits, sizeof(v));
    return true;
}

void appendCommonHeader(std::vector<u_char>& out, std::uint8_t type,
                        std::uint16_t header_bytes, std::uint32_t frame_id,
                        std::uint32_t keyframe_id, const cv::Size& original_size) {
    appendU32(out, kMagic); appendU8(out, kVersion); appendU8(out, type);
    appendU16(out, header_bytes); appendU32(out, frame_id); appendU32(out, keyframe_id);
    appendU16(out, static_cast<std::uint16_t>(original_size.width));
    appendU16(out, static_cast<std::uint16_t>(original_size.height));
}

struct CommonHeader {
    std::uint8_t type = 0;
    std::uint16_t header_bytes = 0;
    std::uint32_t frame_id = 0;
    std::uint32_t keyframe_id = 0;
    cv::Size original_size;
};

bool readCommonHeader(const std::vector<u_char>& data, std::size_t& pos, CommonHeader& h) {
    std::uint32_t magic = 0; std::uint8_t version = 0;
    std::uint16_t width = 0, height = 0;
    if (!readU32(data, pos, magic) || !readU8(data, pos, version) ||
        !readU8(data, pos, h.type) || !readU16(data, pos, h.header_bytes) ||
        !readU32(data, pos, h.frame_id) || !readU32(data, pos, h.keyframe_id) ||
        !readU16(data, pos, width) || !readU16(data, pos, height)) return false;
    if (magic != kMagic || version != kVersion || h.header_bytes < kCommonHeaderBytes) return false;
    h.original_size = cv::Size(width, height);
    return width > 0 && height > 0;
}

bool frameIdNewer(std::uint32_t a, std::uint32_t b) {
    return static_cast<std::int32_t>(a - b) > 0;
}

cv::Mat toGray8(const cv::Mat& image) {
    cv::Mat src8;
    if (image.depth() == CV_8U) src8 = image; else image.convertTo(src8, CV_8U);
    cv::Mat gray;
    if (src8.channels() == 1) gray = src8;
    else if (src8.channels() == 3) cv::cvtColor(src8, gray, cv::COLOR_BGR2GRAY);
    else if (src8.channels() == 4) cv::cvtColor(src8, gray, cv::COLOR_BGRA2GRAY);
    return gray;
}

cv::Mat jpegInput8(const cv::Mat& image) {
    cv::Mat src8;
    if (image.depth() == CV_8U) src8 = image; else image.convertTo(src8, CV_8U);
    if (src8.channels() == 1 || src8.channels() == 3) return src8;
    cv::Mat bgr;
    if (src8.channels() == 4) cv::cvtColor(src8, bgr, cv::COLOR_BGRA2BGR);
    return bgr;
}

std::vector<cv::Point2f> selectGridFeatures(const cv::Mat& gray) {
    std::vector<cv::Point2f> points;
    points.reserve(kFeatureGridX * kFeatureGridY * kFeaturesPerCell);
    for (int gy = 0; gy < kFeatureGridY; ++gy) {
        const int y0 = gy * gray.rows / kFeatureGridY;
        const int y1 = (gy + 1) * gray.rows / kFeatureGridY;
        for (int gx = 0; gx < kFeatureGridX; ++gx) {
            const int x0 = gx * gray.cols / kFeatureGridX;
            const int x1 = (gx + 1) * gray.cols / kFeatureGridX;
            const cv::Rect roi(x0, y0, x1 - x0, y1 - y0);
            if (roi.width < 7 || roi.height < 7) continue;
            std::vector<cv::Point2f> local;
            cv::goodFeaturesToTrack(gray(roi), local, kFeaturesPerCell,
                                    0.01, 7.0, cv::noArray(), 7, false, 0.04);
            for (cv::Point2f p : local) {
                p.x += static_cast<float>(x0); p.y += static_cast<float>(y0);
                points.push_back(p);
            }
        }
    }
    return points;
}

cv::Point2f applyAffine(const cv::Mat& affine, const cv::Point2f& p) {
    return cv::Point2f(
        static_cast<float>(affine.at<double>(0,0)*p.x + affine.at<double>(0,1)*p.y + affine.at<double>(0,2)),
        static_cast<float>(affine.at<double>(1,0)*p.x + affine.at<double>(1,1)*p.y + affine.at<double>(1,2)));
}

int quantizeTo8(int value, int max_value) {
    value = std::max(8, value); value = (value / 8) * 8;
    if (max_value >= 8) value = std::min(value, (max_value / 8) * 8);
    return std::max(8, value);
}
cv::Size scaledSize8(const cv::Size& source, double scale) {
    scale = std::clamp(scale, 0.01, 1.0);
    return cv::Size(quantizeTo8(static_cast<int>(std::floor(source.width * scale)), source.width),
                    quantizeTo8(static_cast<int>(std::floor(source.height * scale)), source.height));
}

bool encodeJpegAtSize(const cv::Mat& source, const cv::Size& size,
                      int quality, std::vector<u_char>& jpeg) {
    cv::Mat small;
    if (source.size() == size) small = source;
    else cv::resize(source, small, size, 0.0, 0.0, cv::INTER_AREA);
    return cv::imencode(".jpg", small, jpeg, {cv::IMWRITE_JPEG_QUALITY, quality});
}

bool encodeJpegToBudget(const cv::Mat& image, int requested_jpeg_bytes,
                        cv::Size& size_hint, int& budget_hint,
                        std::vector<u_char>& jpeg, cv::Size& encoded_size) {
    const cv::Mat source = jpegInput8(image);
    if (source.empty()) return false;
    const int budget = std::max(1, requested_jpeg_bytes);
    constexpr int quality = 85;

    cv::Size candidate;
    if (budget_hint == budget && size_hint.width >= 8 && size_hint.height >= 8) candidate = size_hint;
    else {
        const double estimated_bpp = source.channels() == 1 ? 0.38 : 0.62;
        const double scale = std::sqrt(static_cast<double>(budget) /
            std::max(1.0, static_cast<double>(source.total()) * estimated_bpp));
        candidate = scaledSize8(source.size(), scale);
    }

    std::vector<u_char> current;
    if (!encodeJpegAtSize(source, candidate, quality, current)) return false;
    for (int iter = 0; iter < 8 && static_cast<int>(current.size()) > budget; ++iter) {
        if (candidate.width <= 8 && candidate.height <= 8) break;
        const double ratio = std::sqrt(static_cast<double>(budget) /
            std::max<std::size_t>(1, current.size())) * 0.96;
        const double current_scale = std::min(static_cast<double>(candidate.width) / source.cols,
                                              static_cast<double>(candidate.height) / source.rows);
        cv::Size next = scaledSize8(source.size(), current_scale * ratio);
        if (next == candidate) {
            const double step_scale = std::max(0.01, current_scale - 8.0 / std::max(source.cols, source.rows));
            next = scaledSize8(source.size(), step_scale);
        }
        if (next == candidate) break;
        candidate = next;
        if (!encodeJpegAtSize(source, candidate, quality, current)) return false;
    }

    if (!current.empty() && static_cast<int>(current.size()) < static_cast<int>(budget * 0.78) &&
        (candidate.width < quantizeTo8(source.cols, source.cols) ||
         candidate.height < quantizeTo8(source.rows, source.rows))) {
        const double grow = std::sqrt(static_cast<double>(budget) /
            std::max<std::size_t>(1, current.size())) * 0.97;
        const double current_scale = std::min(static_cast<double>(candidate.width) / source.cols,
                                              static_cast<double>(candidate.height) / source.rows);
        const cv::Size larger = scaledSize8(source.size(), current_scale * grow);
        if (larger != candidate) {
            std::vector<u_char> trial;
            if (encodeJpegAtSize(source, larger, quality, trial) &&
                static_cast<int>(trial.size()) <= budget && trial.size() > current.size()) {
                candidate = larger; current.swap(trial);
            }
        }
    }

    if (static_cast<int>(current.size()) > budget && candidate.width <= 8 && candidate.height <= 8) {
        for (int q : {70, 50, 30, 10}) {
            if (!encodeJpegAtSize(source, candidate, q, current)) return false;
            if (static_cast<int>(current.size()) <= budget) break;
        }
    }
    if (current.empty() || current.size() > std::numeric_limits<std::uint32_t>::max()) return false;
    jpeg = std::move(current); encoded_size = candidate; size_hint = candidate; budget_hint = budget;
    return true;
}

std::vector<u_char> serializeKeyframeChunk(std::uint32_t frame_id, const cv::Size& original_size,
                                           const cv::Size& jpeg_size, std::uint32_t jpeg_bytes,
                                           std::uint16_t chunk_index, std::uint16_t chunk_count,
                                           std::uint32_t chunk_offset, const u_char* chunk_data,
                                           std::uint16_t chunk_bytes) {
    std::vector<u_char> packet; packet.reserve(kKeyframeChunkHeaderBytes + chunk_bytes);
    appendCommonHeader(packet, kPacketKeyframeChunk, static_cast<std::uint16_t>(kKeyframeChunkHeaderBytes),
                       frame_id, frame_id, original_size);
    appendU16(packet, static_cast<std::uint16_t>(jpeg_size.width));
    appendU16(packet, static_cast<std::uint16_t>(jpeg_size.height));
    appendU16(packet, chunk_index); appendU16(packet, chunk_count);
    appendU32(packet, jpeg_bytes); appendU32(packet, chunk_offset);
    appendU16(packet, chunk_bytes); appendU16(packet, 0);
    packet.insert(packet.end(), chunk_data, chunk_data + chunk_bytes);
    return packet;
}

std::vector<u_char> serializePatch(const PatchData& patch) {
    const std::size_t mesh_bytes = patch.mesh.size() * 2 * sizeof(float);
    const std::size_t header_bytes = kCommonHeaderBytes + 4 + 6 * sizeof(float) + mesh_bytes;
    std::vector<u_char> packet; packet.reserve(header_bytes);
    appendCommonHeader(packet, kPacketPatch, static_cast<std::uint16_t>(header_bytes),
                       patch.frame_id, patch.keyframe_id, patch.original_size);
    appendU8(packet, patch.grid_x); appendU8(packet, patch.grid_y); appendU16(packet, 0);
    for (float v : patch.affine) appendFloat(packet, v);
    for (const cv::Point2f& p : patch.mesh) { appendFloat(packet, p.x); appendFloat(packet, p.y); }
    return packet;
}

bool parsePatch(const std::vector<u_char>& data, const CommonHeader& h, std::size_t pos, PatchData& p) {
    std::uint8_t gx = 0, gy = 0; std::uint16_t reserved = 0;
    if (!readU8(data, pos, gx) || !readU8(data, pos, gy) || !readU16(data, pos, reserved) || gx == 0 || gy == 0) return false;
    const std::size_t mesh_count = static_cast<std::size_t>(gx) * gy;
    const std::size_t expected = kCommonHeaderBytes + 4 + 6 * sizeof(float) + mesh_count * 2 * sizeof(float);
    if (h.header_bytes != expected || expected != data.size()) return false;
    p.frame_id = h.frame_id; p.keyframe_id = h.keyframe_id; p.original_size = h.original_size; p.grid_x = gx; p.grid_y = gy;
    for (float& v : p.affine) if (!readFloat(data, pos, v)) return false;
    p.mesh.resize(mesh_count);
    for (cv::Point2f& v : p.mesh) if (!readFloat(data, pos, v.x) || !readFloat(data, pos, v.y)) return false;
    return true;
}

template<int Channels>
cv::Mat fillOutsideImpl(const cv::Mat& image, const cv::Mat& valid) {
    cv::Mat out = image.clone();
    if (cv::countNonZero(valid) == 0) return out;
    cv::Mat state(image.size(), CV_8U, cv::Scalar(0));
    for (int y = 0; y < image.rows; ++y) {
        const unsigned char* vm = valid.ptr<unsigned char>(y);
        unsigned char* sm = state.ptr<unsigned char>(y);
        for (int x = 0; x < image.cols; ++x) if (vm[x]) sm[x] = 2;
    }
    static const int dx[8] = {-1,0,1,-1,1,-1,0,1};
    static const int dy[8] = {-1,-1,-1,0,0,1,1,1};
    std::vector<cv::Point> queue; queue.reserve(image.total());
    for (int y = 0; y < image.rows; ++y) for (int x = 0; x < image.cols; ++x) {
        if (state.at<unsigned char>(y,x) != 0) continue;
        for (int k = 0; k < 8; ++k) {
            const int nx=x+dx[k], ny=y+dy[k];
            if (nx<0 || ny<0 || nx>=image.cols || ny>=image.rows) continue;
            if (state.at<unsigned char>(ny,nx) == 2) { state.at<unsigned char>(y,x)=1; queue.emplace_back(x,y); break; }
        }
    }
    std::size_t head=0;
    while (head < queue.size()) {
        const cv::Point p=queue[head++];
        if constexpr (Channels == 1) {
            int sum=0,count=0;
            for (int k=0;k<8;++k) { const int nx=p.x+dx[k],ny=p.y+dy[k];
                if(nx<0||ny<0||nx>=image.cols||ny>=image.rows) continue;
                if(state.at<unsigned char>(ny,nx)==2){sum+=out.at<unsigned char>(ny,nx);++count;}}
            if(count) out.at<unsigned char>(p.y,p.x)=static_cast<unsigned char>((sum+count/2)/count);
        } else {
            cv::Vec3i sum(0,0,0); int count=0;
            for (int k=0;k<8;++k) { const int nx=p.x+dx[k],ny=p.y+dy[k];
                if(nx<0||ny<0||nx>=image.cols||ny>=image.rows) continue;
                if(state.at<unsigned char>(ny,nx)==2){const cv::Vec3b v=out.at<cv::Vec3b>(ny,nx);sum+=cv::Vec3i(v[0],v[1],v[2]);++count;}}
            if(count) out.at<cv::Vec3b>(p.y,p.x)=cv::Vec3b((sum[0]+count/2)/count,(sum[1]+count/2)/count,(sum[2]+count/2)/count);
        }
        state.at<unsigned char>(p.y,p.x)=2;
        for(int k=0;k<8;++k){const int nx=p.x+dx[k],ny=p.y+dy[k];
            if(nx<0||ny<0||nx>=image.cols||ny>=image.rows) continue;
            unsigned char& st=state.at<unsigned char>(ny,nx); if(st==0){st=1;queue.emplace_back(nx,ny);}}
    }
    cv::Mat previous=out.clone();
    for(int pass=0;pass<2;++pass){out.copyTo(previous);
        for(int y=0;y<image.rows;++y) for(int x=0;x<image.cols;++x){
            if(valid.at<unsigned char>(y,x)) continue;
            if constexpr(Channels==1){int sum=0,count=0;for(int k=0;k<8;++k){const int nx=x+dx[k],ny=y+dy[k];if(nx<0||ny<0||nx>=image.cols||ny>=image.rows)continue;sum+=previous.at<unsigned char>(ny,nx);++count;}if(count)out.at<unsigned char>(y,x)=static_cast<unsigned char>((sum+count/2)/count);}
            else{cv::Vec3i sum(0,0,0);int count=0;for(int k=0;k<8;++k){const int nx=x+dx[k],ny=y+dy[k];if(nx<0||ny<0||nx>=image.cols||ny>=image.rows)continue;const cv::Vec3b v=previous.at<cv::Vec3b>(ny,nx);sum+=cv::Vec3i(v[0],v[1],v[2]);++count;}if(count)out.at<cv::Vec3b>(y,x)=cv::Vec3b((sum[0]+count/2)/count,(sum[1]+count/2)/count,(sum[2]+count/2)/count);}
        }}
    return out;
}
cv::Mat fillOutside(const cv::Mat& image, const cv::Mat& valid) {
    if (cv::countNonZero(valid) == static_cast<int>(valid.total())) return image;
    if (image.type() == CV_8UC1) return fillOutsideImpl<1>(image, valid);
    if (image.type() == CV_8UC3) return fillOutsideImpl<3>(image, valid);
    return image;
}

} // namespace

void Encoder::setReference(const cv::Mat& gray, std::uint32_t frame_id) {
    reference_gray_ = gray.clone(); reference_features_ = selectGridFeatures(reference_gray_);
    reference_pyramid_.clear();
    reference_max_level_ = cv::buildOpticalFlowPyramid(reference_gray_, reference_pyramid_,
        kLkWindow, kLkMaxLevel, true, cv::BORDER_REFLECT_101, cv::BORDER_CONSTANT, false);
    keyframe_id_ = frame_id; frames_since_keyframe_ = 0; have_reference_ = true;
}

bool Encoder::emitKeyframe(const cv::Mat& image, const cv::Mat& gray,
                           int desired_jpeg_size, std::uint32_t frame_id) {
    std::vector<u_char> jpeg; cv::Size jpeg_size;
    if (!encodeJpegToBudget(image, desired_jpeg_size, jpeg_size_hint_, jpeg_budget_hint_, jpeg, jpeg_size)) return false;
    if (jpeg.empty() || jpeg.size() > std::numeric_limits<std::uint32_t>::max()) return false;
    const std::size_t count_size = (jpeg.size() + kKeyframeChunkPayloadBytes - 1) / kKeyframeChunkPayloadBytes;
    if (count_size == 0 || count_size > std::numeric_limits<std::uint16_t>::max()) return false;
    const std::uint16_t chunk_count = static_cast<std::uint16_t>(count_size);
    const std::uint32_t jpeg_bytes = static_cast<std::uint32_t>(jpeg.size());
    for (std::uint16_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        const std::size_t offset = static_cast<std::size_t>(chunk_index) * kKeyframeChunkPayloadBytes;
        const std::size_t bytes = std::min(kKeyframeChunkPayloadBytes, jpeg.size() - offset);
        std::vector<u_char> packet = serializeKeyframeChunk(frame_id, image.size(), jpeg_size, jpeg_bytes,
            chunk_index, chunk_count, static_cast<std::uint32_t>(offset), jpeg.data() + offset,
            static_cast<std::uint16_t>(bytes));
        if (packet.size() > kMaxUdpPacketBytes) return false;
        output_queue_.push_back(std::move(packet));
    }
    input_size_ = image.size(); setReference(gray, frame_id); return true;
}

bool Encoder::estimatePatch(const cv::Mat& current_gray, std::uint32_t frame_id, PatchData& patch) {
    if (!have_reference_ || reference_features_.size() < 4) return false;
    current_pyramid_.clear();
    const int current_max_level = cv::buildOpticalFlowPyramid(current_gray, current_pyramid_,
        kLkWindow, kLkMaxLevel, true, cv::BORDER_REFLECT_101, cv::BORDER_CONSTANT, false);
    const int max_level = std::min(reference_max_level_, current_max_level);

    lk_forward_.clear(); lk_status_forward_.clear(); lk_error_forward_.clear();
    cv::calcOpticalFlowPyrLK(reference_pyramid_, current_pyramid_, reference_features_, lk_forward_,
        lk_status_forward_, lk_error_forward_, kLkWindow, max_level,
        cv::TermCriteria(cv::TermCriteria::COUNT|cv::TermCriteria::EPS,40,0.01),0,1e-4);

    std::vector<cv::Point2f> g0,g1; g0.reserve(reference_features_.size()); g1.reserve(reference_features_.size());
    for(std::size_t i=0;i<reference_features_.size();++i) if(i<lk_status_forward_.size()&&lk_status_forward_[i]&&i<lk_error_forward_.size()&&lk_error_forward_[i]<kLkForwardErrorMax){g0.push_back(reference_features_[i]);g1.push_back(lk_forward_[i]);}
    if(g0.size()<4) return false;

    lk_back_.clear(); lk_status_back_.clear(); lk_error_back_.clear();
    cv::calcOpticalFlowPyrLK(current_pyramid_, reference_pyramid_, g1, lk_back_, lk_status_back_, lk_error_back_,
        kLkWindow,max_level,cv::TermCriteria(cv::TermCriteria::COUNT|cv::TermCriteria::EPS,40,0.01));
    std::vector<cv::Point2f> p0,p1; p0.reserve(g0.size()); p1.reserve(g0.size());
    for(std::size_t i=0;i<g0.size();++i){if(i>=lk_status_back_.size()||!lk_status_back_[i])continue;if(cv::norm(lk_back_[i]-g0[i])>kLkBackwardErrorMax)continue;p0.push_back(g0[i]);p1.push_back(g1[i]);}
    if(p0.size()<4) return false;

    cv::Mat inliers; cv::Mat affine=cv::estimateAffinePartial2D(p0,p1,inliers,cv::RANSAC,2.0,3000,0.995,10);
    if(affine.empty()) return false; if(affine.type()!=CV_64F) affine.convertTo(affine,CV_64F);
    patch.frame_id=frame_id; patch.keyframe_id=keyframe_id_; patch.original_size=input_size_; patch.grid_x=kMeshGridX; patch.grid_y=kMeshGridY;
    patch.mesh.assign(kMeshGridX*kMeshGridY,cv::Point2f(0,0));
    patch.affine={static_cast<float>(affine.at<double>(0,0)),static_cast<float>(affine.at<double>(0,1)),static_cast<float>(affine.at<double>(0,2)),static_cast<float>(affine.at<double>(1,0)),static_cast<float>(affine.at<double>(1,1)),static_cast<float>(affine.at<double>(1,2))};

    std::vector<cv::Point2f> q,residual; q.reserve(p0.size()); residual.reserve(p0.size());
    const unsigned char* ip=inliers.empty()?nullptr:inliers.ptr<unsigned char>();
    for(std::size_t i=0;i<p0.size();++i){if(ip&&!ip[i])continue;const cv::Point2f qi=applyAffine(affine,p0[i]);const cv::Point2f ri=p1[i]-qi;if(cv::norm(ri)<kResidualMax){q.push_back(qi);residual.push_back(ri);}}
    const double sigma=std::max(16.0,0.20*std::max(input_size_.width,input_size_.height)); const double inv2s2=1.0/(2.0*sigma*sigma);
    for(int iy=0;iy<kMeshGridY;++iy){const float y=static_cast<float>(iy)*(input_size_.height-1)/(kMeshGridY-1);
        for(int ix=0;ix<kMeshGridX;++ix){const float x=static_cast<float>(ix)*(input_size_.width-1)/(kMeshGridX-1);cv::Point2d sum(0,0);double sw=0;
            for(std::size_t k=0;k<q.size();++k){const double dx=q[k].x-x,dy=q[k].y-y,w=std::exp(-(dx*dx+dy*dy)*inv2s2);sum.x+=w*residual[k].x;sum.y+=w*residual[k].y;sw+=w;}
            if(sw>1e-9)patch.mesh[iy*kMeshGridX+ix]=cv::Point2f(static_cast<float>(sum.x/sw),static_cast<float>(sum.y/sw));}}
    return true;
}

void Encoder::pushImage(cv::Mat& image,int desired_jpeg_size,int keyframe_once_in_N){
    if(image.empty())return;if(image.cols>std::numeric_limits<std::uint16_t>::max()||image.rows>std::numeric_limits<std::uint16_t>::max())return;
    const cv::Mat gray=toGray8(image);if(gray.empty())return;const std::uint32_t frame_id=next_frame_id_++;const int period=std::max(1,keyframe_once_in_N);
    const bool size_changed=have_reference_&&image.size()!=input_size_;const bool periodic=!have_reference_||period==1||frames_since_keyframe_>=period-1;
    if(size_changed){have_reference_=false;jpeg_size_hint_=cv::Size();jpeg_budget_hint_=0;}
    if(!have_reference_||periodic){emitKeyframe(image,gray,desired_jpeg_size,frame_id);return;}
    PatchData patch;if(!estimatePatch(gray,frame_id,patch)){emitKeyframe(image,gray,desired_jpeg_size,frame_id);return;}
    std::vector<u_char> packet=serializePatch(patch);if(packet.size()<=kMaxUdpPacketBytes){output_queue_.push_back(std::move(packet));++frames_since_keyframe_;}else emitKeyframe(image,gray,desired_jpeg_size,frame_id);
}

bool Encoder::getNextChunk(std::vector<u_char>& data){if(output_queue_.empty())return false;data=std::move(output_queue_.front());output_queue_.pop_front();return true;}

void Decoder::resetPendingKeyframe() {
    pending_keyframe_ = KeyframeAssembly{};
    pending_patch_queue_.clear();
}

void Decoder::pushData(const std::vector<u_char>& data){
    if(data.size()<kCommonHeaderBytes||data.size()>kMaxUdpPacketBytes)return;std::size_t pos=0;CommonHeader h;if(!readCommonHeader(data,pos,h)||h.header_bytes>data.size())return;
    if(h.type==kPacketKeyframeChunk){
        if(h.header_bytes!=kKeyframeChunkHeaderBytes||h.keyframe_id!=h.frame_id)return;
        std::uint16_t jw=0,jh=0,chunk_index=0,chunk_count=0,chunk_bytes=0,reserved=0;std::uint32_t jpeg_bytes=0,chunk_offset=0;
        if(!readU16(data,pos,jw)||!readU16(data,pos,jh)||!readU16(data,pos,chunk_index)||!readU16(data,pos,chunk_count)||
           !readU32(data,pos,jpeg_bytes)||!readU32(data,pos,chunk_offset)||!readU16(data,pos,chunk_bytes)||!readU16(data,pos,reserved))return;
        if(jw==0||jh==0||jpeg_bytes==0||jpeg_bytes>kMaxAcceptedJpegBytes||chunk_count==0||chunk_index>=chunk_count||
           static_cast<std::size_t>(h.header_bytes)+chunk_bytes!=data.size())return;
        const std::size_t expected_count=(static_cast<std::size_t>(jpeg_bytes)+kKeyframeChunkPayloadBytes-1)/kKeyframeChunkPayloadBytes;
        if(expected_count!=chunk_count)return;
        const std::size_t expected_offset=static_cast<std::size_t>(chunk_index)*kKeyframeChunkPayloadBytes;
        if(expected_offset>=jpeg_bytes)return;
        const std::size_t expected_bytes=std::min(kKeyframeChunkPayloadBytes,static_cast<std::size_t>(jpeg_bytes)-expected_offset);
        if(expected_offset!=chunk_offset||expected_bytes!=chunk_bytes||expected_offset+expected_bytes>jpeg_bytes)return;
        if(have_keyframe_){if(h.frame_id==keyframe_id_)return;if(!frameIdNewer(h.frame_id,keyframe_id_))return;}
        if(pending_keyframe_.active&&h.frame_id!=pending_keyframe_.frame_id){if(!frameIdNewer(h.frame_id,pending_keyframe_.frame_id))return;resetPendingKeyframe();}
        if(!pending_keyframe_.active){
            pending_keyframe_.active=true;pending_keyframe_.frame_id=h.frame_id;pending_keyframe_.original_size=h.original_size;
            pending_keyframe_.jpeg_size=cv::Size(jw,jh);pending_keyframe_.jpeg_bytes=jpeg_bytes;pending_keyframe_.chunk_count=chunk_count;
            pending_keyframe_.received_count=0;pending_keyframe_.bytes.resize(jpeg_bytes);pending_keyframe_.received.assign(chunk_count,0);
        }else if(pending_keyframe_.original_size!=h.original_size||pending_keyframe_.jpeg_size!=cv::Size(jw,jh)||
                 pending_keyframe_.jpeg_bytes!=jpeg_bytes||pending_keyframe_.chunk_count!=chunk_count)return;
        if(!pending_keyframe_.received[chunk_index]){
            std::copy(data.begin()+h.header_bytes,data.end(),pending_keyframe_.bytes.begin()+chunk_offset);
            pending_keyframe_.received[chunk_index]=1;++pending_keyframe_.received_count;
        }
        if(pending_keyframe_.received_count==pending_keyframe_.chunk_count){
            current_jpeg_=std::move(pending_keyframe_.bytes);original_size_=pending_keyframe_.original_size;keyframe_id_=pending_keyframe_.frame_id;
            have_keyframe_=true;keyframe_changed_=true;decoded_keyframe_.release();patch_queue_.clear();
            while(!pending_patch_queue_.empty()){patch_queue_.push_back(std::move(pending_patch_queue_.front()));pending_patch_queue_.pop_front();}
            pending_keyframe_=KeyframeAssembly{};
        }
        return;
    }
    if(h.type==kPacketPatch){
        PatchData patch;if(!parsePatch(data,h,pos,patch))return;
        if(have_keyframe_&&h.keyframe_id==keyframe_id_&&h.original_size==original_size_){patch_queue_.push_back(std::move(patch));return;}
        if(pending_keyframe_.active&&h.keyframe_id==pending_keyframe_.frame_id&&h.original_size==pending_keyframe_.original_size){
            if(pending_patch_queue_.size()>=kMaxPendingPatches)pending_patch_queue_.pop_front();pending_patch_queue_.push_back(std::move(patch));
        }
    }
}

bool Decoder::updateKeyframe(std::vector<u_char>& jpeg_data){if(!keyframe_changed_)return false;jpeg_data=current_jpeg_;keyframe_changed_=false;return true;}
bool Decoder::getNextPatch(std::vector<PatchData>& patch){if(patch_queue_.empty())return false;patch.clear();patch.push_back(std::move(patch_queue_.front()));patch_queue_.pop_front();return true;}

cv::Mat Decoder::getDecodedKeyframe(const std::vector<u_char>& jpeg_data){
    if(!have_keyframe_||original_size_.width<=0||original_size_.height<=0)return cv::Mat();
    if(!decoded_keyframe_.empty())return decoded_keyframe_;
    const std::vector<u_char>& bytes=jpeg_data.empty()?current_jpeg_:jpeg_data;if(bytes.empty())return cv::Mat();
    cv::Mat encoded(1,static_cast<int>(bytes.size()),CV_8U,const_cast<u_char*>(bytes.data()));cv::Mat decoded=cv::imdecode(encoded,cv::IMREAD_UNCHANGED);
    if(decoded.empty())return cv::Mat();if(decoded.size()!=original_size_)cv::resize(decoded,decoded,original_size_,0,0,cv::INTER_LINEAR);decoded_keyframe_=decoded;return decoded_keyframe_;
}

void Decoder::render(cv::Mat& destination,const std::vector<PatchData>& patch,const std::vector<u_char>& jpeg_data){
    cv::Mat keyframe=getDecodedKeyframe(jpeg_data);if(keyframe.empty()){destination.release();return;}if(patch.empty()){keyframe.copyTo(destination);return;}
    const PatchData&p=patch.back();if(p.keyframe_id!=keyframe_id_||p.original_size!=original_size_||p.grid_x==0||p.grid_y==0||p.mesh.size()!=static_cast<std::size_t>(p.grid_x)*p.grid_y){destination.release();return;}
    cv::Mat grid(p.grid_y,p.grid_x,CV_32FC2);for(int y=0;y<p.grid_y;++y){cv::Vec2f*row=grid.ptr<cv::Vec2f>(y);for(int x=0;x<p.grid_x;++x){const cv::Point2f v=p.mesh[y*p.grid_x+x];row[x]=cv::Vec2f(v.x,v.y);}}
    cv::resize(grid,dense_mesh_,original_size_,0,0,cv::INTER_CUBIC);
    const float a00=p.affine[0],a01=p.affine[1],a02=p.affine[2],a10=p.affine[3],a11=p.affine[4],a12=p.affine[5],det=a00*a11-a01*a10;if(std::abs(det)<1e-9f){destination.release();return;}
    const float i00=a11/det,i01=-a01/det,i10=-a10/det,i11=a00/det,i02=-(i00*a02+i01*a12),i12=-(i10*a02+i11*a12);
    map_x_.create(original_size_,CV_32F);map_y_.create(original_size_,CV_32F);valid_mask_.create(original_size_,CV_8U);valid_mask_.setTo(cv::Scalar(0));
    cv::parallel_for_(cv::Range(0,original_size_.height),[&](const cv::Range&r){for(int y=r.start;y<r.end;++y){const cv::Vec2f*dr=dense_mesh_.ptr<cv::Vec2f>(y);float*mx=map_x_.ptr<float>(y);float*my=map_y_.ptr<float>(y);unsigned char*vm=valid_mask_.ptr<unsigned char>(y);for(int x=0;x<original_size_.width;++x){const float tx=x-dr[x][0],ty=y-dr[x][1],sx=i00*tx+i01*ty+i02,sy=i10*tx+i11*ty+i12;mx[x]=sx;my[x]=sy;if(sx>=0&&sx<keyframe.cols-1&&sy>=0&&sy<keyframe.rows-1)vm[x]=255;}}});
    cv::remap(keyframe,destination,map_x_,map_y_,cv::INTER_LINEAR,cv::BORDER_CONSTANT,cv::Scalar(0));destination=fillOutside(destination,valid_mask_);
}

} // namespace affinecodec
