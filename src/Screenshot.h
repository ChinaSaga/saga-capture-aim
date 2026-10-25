#pragma once
#include "Inference.h"
#include "EncodedImage.h"
#include "FilePaths.h"
#include <opencv2/imgcodecs.hpp>

namespace screenshot {
// Both local capture and top-down UDP BMP can be encoded through a stride-aware
// view. Only compressed/bottom-up inputs need an allocated decoded image.
inline bool encode(const CaptureFrame& frame, const std::vector<unsigned char>& bytes,
    std::vector<unsigned char>& jpeg) {
    static const std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, 95};
    cv::Mat pixels;
    encoded_image::BgrView view;
    if (frame.owner) {
        if (!encoded_image::topDownBgr(frame.bmp, frame.size > 0 ? frame.size : 0, view)) return false;
        pixels = cv::Mat(view.height, view.width, CV_8UC3,
            const_cast<unsigned char*>(frame.bmp + view.offset), view.stride);
    } else if (encoded_image::topDownBgr(bytes.data(), bytes.size(), view)) {
        pixels = cv::Mat(view.height, view.width, CV_8UC3,
            const_cast<unsigned char*>(bytes.data() + view.offset), view.stride);
    } else if (!bytes.empty()) pixels = cv::imdecode(bytes, cv::IMREAD_COLOR);
    return !pixels.empty() && cv::imencode(".jpg", pixels, jpeg, params);
}

class Writer {
public:
    bool save(const std::filesystem::path& root, const std::string& model, long long timestamp,
        const std::vector<unsigned char>& jpeg) {
        if (jpeg.empty()) return false;
        if (!ready_ || root != root_ || model != model_) {
            root_ = root; model_ = model;
            directory_ = file_paths::io(root / file_paths::wide(model));
            ready_ = makeDirectory();
            if (!ready_) return false;
        }
        const auto path = directory_ / (std::to_wstring(timestamp) + L".jpg");
        // directory_ is already an absolute extended Unicode path. Avoid
        // normalizing it and attempting CreateDirectory for every screenshot.
        FILE* file = _wfopen(path.c_str(), L"wb");
        if (!file) {
            // Recover if the user moved/deleted the screenshot folder.
            ready_ = makeDirectory();
            if (ready_) file = _wfopen(path.c_str(), L"wb");
        }
        if (!file) return false;
        const bool written = fwrite(jpeg.data(), 1, jpeg.size(), file) == jpeg.size();
        const bool closed = fclose(file) == 0;
        if (!written || !closed) { DeleteFileW(path.c_str()); return false; }
        return true;
    }
private:
    bool makeDirectory() const {
        if (CreateDirectoryW(directory_.c_str(), nullptr)) return true;
        if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
        const auto attributes = GetFileAttributesW(directory_.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    std::filesystem::path root_, directory_;
    std::string model_;
    bool ready_ = false;
};
}
