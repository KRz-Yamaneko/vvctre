// Copyright 2020 vvctre project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/assert.h"
#include "common/file_util.h"
#include "common/stb_image.h"
#include "common/stb_image_resize.h"
#include "vvctre/camera/image.h"
#include "vvctre/camera/util.h"

namespace Camera {

ImageCamera::ImageCamera(const std::string& file) {
    image = stbi_load(file.c_str(), &file_width, &file_height, nullptr, 3);
    if (image == nullptr) {
        LOG_ERROR(Service_CAM, "Failed to load image");
    }
}

ImageCamera::~ImageCamera() {
    if (image != nullptr) {
        free(image);
    }
}

void ImageCamera::SetResolution(const Service::CAM::Resolution& resolution) {
    requested_width = resolution.width;
    requested_height = resolution.height;
}

void ImageCamera::SetFormat(Service::CAM::OutputFormat format) {
    this->format = format;
}

std::vector<u16> ImageCamera::ReceiveFrame() {
    if (image == nullptr) {
        // Note: 0x80008000 stands for two black pixels in YUV422
        std::vector<u16> frame(requested_width * requested_height,
                               format == Service::CAM::OutputFormat::RGB565 ? 0 : 0x8000);
        return frame;
    }

    std::vector<unsigned char> resized(requested_width * requested_height * 3, 0);
    ASSERT(stbir_resize_uint8(image, file_width, file_height, 0, resized.data(), requested_width,
                              requested_height, 0, 3) == 1);

    if (format == Service::CAM::OutputFormat::RGB565) {
        std::vector<u16> frame(requested_width * requested_height, 0);
        std::size_t resized_offset = 0;
        std::size_t pixel = 0;

        while (resized_offset < resized.size()) {
            unsigned char r = resized[resized_offset++];
            unsigned char g = resized[resized_offset++];
            unsigned char b = resized[resized_offset++];
            frame[pixel++] = ((r & 0b11111000) << 8) | ((g & 0b11111100) << 3) | (b >> 3);
        }

        return frame;
    } else if (format == Service::CAM::OutputFormat::YUV422) {
        return convert_rgb888_to_yuyv(resized, requested_width, requested_height);
    }

    UNIMPLEMENTED();
    return {};
}

void ImageCamera::StartCapture() {}
void ImageCamera::StopCapture() {}

void ImageCamera::SetFlip(Service::CAM::Flip flip) {
    UNIMPLEMENTED();
}

void ImageCamera::SetEffect(Service::CAM::Effect effect) {
    UNIMPLEMENTED();
}

bool ImageCamera::IsPreviewAvailable() {
    return false;
}

std::unique_ptr<CameraInterface> ImageCameraFactory::Create(const std::string& file,
                                                            const Service::CAM::Flip& /*flip*/) {
    return std::make_unique<ImageCamera>(file);
}

} // namespace Camera