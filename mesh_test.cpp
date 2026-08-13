#include <opencv2/opencv.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct TrackSet { std::vector<cv::Point2f> p0, p1; };

static bool isImageFile(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c){ return char(std::tolower(c)); });
    return e==".jpg" || e==".jpeg" || e==".png" || e==".bmp" || e==".tif" || e==".tiff";
}
static std::vector<fs::path> listImages(const fs::path& dir) {
    std::vector<fs::path> v;
    for (auto& e: fs::directory_iterator(dir)) if (e.is_regular_file() && isImageFile(e.path())) v.push_back(e.path());
    std::sort(v.begin(),v.end()); return v;
}

static TrackSet trackLK(const cv::Mat& a,const cv::Mat& b) {
    std::vector<cv::Point2f> p0;
    cv::goodFeaturesToTrack(a,p0,600,0.01,8.0,cv::noArray(),7,false,0.04);
    TrackSet out; if(p0.empty()) return out;
    std::vector<cv::Point2f> p1; std::vector<uchar> st; std::vector<float> err;
    cv::calcOpticalFlowPyrLK(a,b,p0,p1,st,err,cv::Size(21,21),4,
        cv::TermCriteria(cv::TermCriteria::COUNT|cv::TermCriteria::EPS,30,0.01),0,1e-4);
    std::vector<cv::Point2f> g0,g1;
    for(size_t i=0;i<p0.size();++i) if(st[i] && err[i]<30.f){g0.push_back(p0[i]);g1.push_back(p1[i]);}
    if(g0.empty()) return out;
    std::vector<cv::Point2f> back; std::vector<uchar> stb; std::vector<float> errb;
    cv::calcOpticalFlowPyrLK(b,a,g1,back,stb,errb,cv::Size(21,21),4,
        cv::TermCriteria(cv::TermCriteria::COUNT|cv::TermCriteria::EPS,30,0.01));
    for(size_t i=0;i<g0.size();++i) if(stb[i] && cv::norm(back[i]-g0[i])<=1.5){out.p0.push_back(g0[i]);out.p1.push_back(g1[i]);}
    return out;
}

static cv::Point2f applyAffine(const cv::Mat& M,const cv::Point2f& p){
    return {(float)(M.at<double>(0,0)*p.x+M.at<double>(0,1)*p.y+M.at<double>(0,2)),
            (float)(M.at<double>(1,0)*p.x+M.at<double>(1,1)*p.y+M.at<double>(1,2))};
}

struct MeshResult {
    cv::Mat affine, grid, warped;
    double affineMAE=0, meshMAE=0;
};

static double maskedMAE(const cv::Mat& x,const cv::Mat& y,const cv::Mat& mask){
    cv::Mat d; cv::absdiff(x,y,d); return cv::mean(d,mask)[0];
}

static MeshResult fitMesh(const cv::Mat& a,const cv::Mat& b,const TrackSet& t,int gx=4,int gy=4,double sigma=80.0){
    MeshResult r; if(t.p0.size()<6) return r;
    cv::Mat inliers;
    r.affine=cv::estimateAffine2D(t.p0,t.p1,inliers,cv::RANSAC,2.0,3000,0.995,10);
    if(r.affine.empty()) return r;

    std::vector<cv::Point2f> q,res;
    for(size_t i=0;i<t.p0.size();++i){
        cv::Point2f qi=applyAffine(r.affine,t.p0[i]);
        cv::Point2f ri=t.p1[i]-qi;
        if(cv::norm(ri)<8.0){q.push_back(qi);res.push_back(ri);}
    }

    r.grid=cv::Mat(gy,gx,CV_32FC2,cv::Scalar(0,0));
    for(int iy=0;iy<gy;++iy){
        float y=(gy==1)?0.f:float(iy)*(b.rows-1)/float(gy-1);
        for(int ix=0;ix<gx;++ix){
            float x=(gx==1)?0.f:float(ix)*(b.cols-1)/float(gx-1);
            cv::Point2d s(0,0); double sw=0;
            for(size_t k=0;k<q.size();++k){
                double dx=q[k].x-x,dy=q[k].y-y;
                double w=std::exp(-(dx*dx+dy*dy)/(2.0*sigma*sigma));
                s.x+=w*res[k].x; s.y+=w*res[k].y; sw+=w;
            }
            if(sw>1e-9) r.grid.at<cv::Vec2f>(iy,ix)=cv::Vec2f((float)(s.x/sw),(float)(s.y/sw));
        }
    }

    cv::Mat dense; cv::resize(r.grid,dense,b.size(),0,0,cv::INTER_CUBIC);
    cv::Mat inv; cv::invertAffineTransform(r.affine,inv);
    cv::Mat mapx(b.size(),CV_32F),mapy(b.size(),CV_32F),valid(b.size(),CV_8U,cv::Scalar(0));
    for(int y=0;y<b.rows;++y){
        const cv::Vec2f* dr=dense.ptr<cv::Vec2f>(y);
        float* mx=mapx.ptr<float>(y); float* my=mapy.ptr<float>(y); uchar* vm=valid.ptr<uchar>(y);
        for(int x=0;x<b.cols;++x){
            double tx=x-dr[x][0], ty=y-dr[x][1];
            double sx=inv.at<double>(0,0)*tx+inv.at<double>(0,1)*ty+inv.at<double>(0,2);
            double sy=inv.at<double>(1,0)*tx+inv.at<double>(1,1)*ty+inv.at<double>(1,2);
            mx[x]=(float)sx; my[x]=(float)sy;
            if(sx>=2 && sx<a.cols-2 && sy>=2 && sy<a.rows-2) vm[x]=255;
        }
    }
    cv::remap(a,r.warped,mapx,mapy,cv::INTER_LINEAR,cv::BORDER_CONSTANT,0);

    cv::Mat wa,amask(a.size(),CV_8U,cv::Scalar(255)),wmask;
    cv::warpAffine(a,wa,r.affine,b.size(),cv::INTER_LINEAR,cv::BORDER_CONSTANT,0);
    cv::warpAffine(amask,wmask,r.affine,b.size(),cv::INTER_NEAREST,cv::BORDER_CONSTANT,0);
    r.affineMAE=maskedMAE(wa,b,wmask);
    r.meshMAE=maskedMAE(r.warped,b,valid);
    return r;
}

static void saveDiag(const fs::path& path,const cv::Mat& a,const cv::Mat& b,const MeshResult& r){
    if(r.affine.empty()) return;
    cv::Mat wa; cv::warpAffine(a,wa,r.affine,b.size());
    cv::Mat da,dm; cv::absdiff(wa,b,da); cv::absdiff(r.warped,b,dm); da*=3; dm*=3;
    auto c3=[](const cv::Mat& g){cv::Mat c;cv::cvtColor(g,c,cv::COLOR_GRAY2BGR);return c;};
    cv::Mat top,bot,canvas;
    cv::hconcat(std::vector<cv::Mat>{c3(b),c3(wa),c3(r.warped)},top);
    cv::hconcat(std::vector<cv::Mat>{c3(a),c3(da),c3(dm)},bot);
    cv::vconcat(top,bot,canvas);
    cv::putText(canvas,cv::format("Affine MAE %.2f   Mesh MAE %.2f",r.affineMAE,r.meshMAE),{10,24},
        cv::FONT_HERSHEY_SIMPLEX,0.55,{255,255,255},2,cv::LINE_AA);
    cv::imwrite(path.string(),canvas);
}

int main(int argc,char** argv){
    if(argc>5){
        std::cerr<<"Usage: mesh_test [image-folder] [output-folder] [grid-x] [grid-y]\n"
                 <<"Defaults: local-data/frames -> output/mesh, grid 4x4\n";
        return 2;
    }
    fs::path in=argc>1?fs::path(argv[1]):fs::path("local-data/frames");
    fs::path out=argc>2?fs::path(argv[2]):fs::path("output/mesh");
    int gx=argc>3?std::stoi(argv[3]):4,gy=argc>4?std::stoi(argv[4]):4;
    fs::create_directories(out);
    if(!fs::exists(in)){
        std::cerr<<"Input folder does not exist: "<<in<<"\n"
                 <<"Create local-data/frames and put the flight images there.\n";
        return 1;
    }
    auto files=listImages(in); if(files.size()<2){std::cerr<<"Need >=2 images in "<<in<<"\n";return 1;}
    std::ofstream csv(out/"mesh_metrics.csv"); csv<<"pair,file0,file1,lk_points,affine_mae,mesh_mae,gain_percent\n";
    double sa=0,sm=0; int n=0;
    for(size_t i=0;i+1<files.size();++i){
        cv::Mat a=cv::imread(files[i].string(),0),b=cv::imread(files[i+1].string(),0); if(a.empty()||b.empty()||a.size()!=b.size())continue;
        TrackSet t=trackLK(a,b); MeshResult r=fitMesh(a,b,t,gx,gy); if(r.affine.empty())continue;
        double gain=100.0*(r.affineMAE-r.meshMAE)/r.affineMAE; sa+=r.affineMAE;sm+=r.meshMAE;++n;
        std::cout<<std::setw(3)<<i<<" LK="<<t.p0.size()<<" affine="<<std::fixed<<std::setprecision(2)<<r.affineMAE<<" mesh="<<r.meshMAE<<" gain="<<gain<<"%\n";
        csv<<i<<','<<files[i].filename().string()<<','<<files[i+1].filename().string()<<','<<t.p0.size()<<','<<r.affineMAE<<','<<r.meshMAE<<','<<gain<<'\n';
        char name[64]; std::snprintf(name,sizeof(name),"%04zu.jpg",i); saveDiag(out/name,a,b,r);
    }
    std::cout<<"\nMean affine MAE="<<(n?sa/n:0)<<" mesh MAE="<<(n?sm/n:0)<<" gain="<<(n?100*(sa-sm)/sa:0)<<"%\n";
    std::cout<<"Diagnostics: "<<out<<"\n";
    return 0;
}
