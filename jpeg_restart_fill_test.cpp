#include "jpeg_restart_fill.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected browser fill cases JSON");
        cv::setNumThreads(2);
        std::ifstream input(argv[1]); nlohmann::json cases; input >> cases;
        unsigned tested = 0;
        for (const auto& item : cases) for (int channels : {1,3,4}) {
            const int w = item.at("width"), h = item.at("height");
            const auto mask = item.at("received").get<std::vector<unsigned char>>();
            const auto pixels = item.at("pixels").get<std::vector<unsigned char>>();
            const auto expected = item.at("expected").get<std::vector<unsigned char>>();
            const auto params = item.at("params").get<std::vector<unsigned char>>();
            // A non-contiguous view also catches accidental assumptions about stride.
            cv::Mat storage(h,w+2,CV_MAKETYPE(CV_8U,channels));
            cv::Mat source = storage(cv::Rect(1,0,w,h));
            for (int y=0;y<h;++y) for (int x=0;x<w;++x) for (int c=0;c<channels;++c)
                source.ptr(y)[x*channels+c] = pixels[(y*w+x)*4+c];
            auto frozen = source.clone();
            const auto result = affinecodec::fillRestartGaps(source,mask,true);
            for (int y=0;y<h;++y) for (int x=0;x<w;++x) for (int c=0;c<channels;++c) {
                const int i=y*w+x, error=std::abs(int(result.ptr(y)[x*channels+c])-int(expected[4*i+c]));
                if (error > (params[2*i+1] && c!=3 ? 1 : 0))
                    throw std::runtime_error(item.at("name").get<std::string>()+": native/browser fill mismatch");
            }
            if (cv::norm(frozen,source,cv::NORM_INF)!=0 ||
                cv::norm(source,affinecodec::fillRestartGaps(source,mask,false),cv::NORM_INF)!=0 ||
                cv::norm(result,affinecodec::fillRestartGaps(result,mask,true),cv::NORM_INF)!=0)
                throw std::runtime_error("source mutated, NN mismatch, or accumulated blur");
            ++tested;
        }
        cv::Mat empty(8,16,CV_8UC3,cv::Scalar::all(0));
        if (cv::norm(empty,affinecodec::fillRestartGaps(empty,{0,0},true),cv::NORM_INF)!=0)
            throw std::runtime_error("empty frame invented data");
        std::cout << "PASS: native/browser fill equivalence, " << tested
                  << " grayscale/RGB/RGBA cases, known pixels, non-contiguous storage, immutable sources\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
