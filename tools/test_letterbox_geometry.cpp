#include "preprocessing.hpp"
#include <iostream>
#include <stdexcept>

static int pythonRound(float value) {
    const int lower = static_cast<int>(std::floor(value));
    const float fraction = value - lower;
    return lower + (fraction > .5f || (fraction == .5f && (lower & 1)));
}
static void check(const cv::Size& source, const cv::Size& target, bool dynamic,
                  yolos::preprocessing::InferenceBuffer& buffer) {
    cv::Mat image(source, CV_8UC3);
    for (int y = 0; y < image.rows; ++y)
        for (int x = 0; x < image.cols; ++x)
            image.at<cv::Vec3b>(y, x) = cv::Vec3b((x*17+y)%256,(x+y*19)%256,(x*11+y*7)%256);
    const float scale = std::min(float(target.width)/source.width, float(target.height)/source.height);
    const int width = pythonRound(source.width*scale), height = pythonRound(source.height*scale);
    const cv::Size actual = dynamic ? cv::Size((width+31)/32*32,(height+31)/32*32) : target;
    const int left = pythonRound((actual.width-width)/2.0f-.1f);
    const int top = pythonRound((actual.height-height)/2.0f-.1f);
    cv::Mat resized;
    cv::resize(image, resized, cv::Size(width,height),0,0,cv::INTER_LINEAR);
    cv::Size observed;
    yolos::preprocessing::letterBoxToBlob(image,buffer,3,target,observed,dynamic);
    if (observed != actual) throw std::runtime_error("Wrong tensor dimensions");
    const size_t plane = size_t(actual.width)*actual.height;
    for(int y=0;y<actual.height;++y) for(int x=0;x<actual.width;++x) for(int c=0;c<3;++c) {
        float expected = 114.f/255.f;
        if (x>=left && x<left+width && y>=top && y<top+height)
            expected = resized.at<cv::Vec3b>(y-top,x-left)[2-c]*(1.f/255.f);
        if (buffer.blob[c*plane+y*actual.width+x] != expected)
            throw std::runtime_error("Pixel content/padding mismatch");
    }
    {
        float observedScale,padX,padY;
        yolos::preprocessing::getScalePad(source,actual,observedScale,padX,padY);
        if(observedScale!=scale || padX!=left || padY!=top)
            throw std::runtime_error("Postprocessing transform differs from applied padding");
        float coordinates[] = {left+100.f*scale,top+50.f*scale};
        yolos::preprocessing::descaleCoordsBatch(coordinates,1,scale,padX,padY);
        if(std::abs(coordinates[0]-100.f)>.0001f || std::abs(coordinates[1]-50.f)>.0001f)
            throw std::runtime_error("Coordinate round trip failed");
        if (!dynamic) {
            std::vector<float> standalone;
            yolos::preprocessing::letterBoxToBlob(image,standalone,3,target,observed);
            // /fp:fast may give scalar and explicit SIMD multiplication one
            // final rounding-bit difference; geometry/padding must still agree.
            for(size_t i=0;i<standalone.size();++i)
                if(std::abs(standalone[i]-buffer.blob[i])>1e-7f)
                    throw std::runtime_error("Buffered and standalone letterbox disagree");
        }
    }
}
int main() {
    try {
    yolos::preprocessing::InferenceBuffer buffer;
    for(int repeat=0;repeat<2;++repeat)
        for(const auto& source : {cv::Size(640,361),cv::Size(361,640),cv::Size(832,417),
             cv::Size(417,832),cv::Size(831,419),cv::Size(416,416),cv::Size(640,480),cv::Size(1920,1080)})
            for(bool dynamic : {false,true}) check(source,cv::Size(416,416),dynamic,buffer);
    std::cout << "32 letterbox layouts passed: independent ties-to-even geometry, exact RGB pixels,\n"
                 "odd padding, non-square images, dynamic shape, buffer reuse and coordinate round trips.\n";
    } catch(const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
