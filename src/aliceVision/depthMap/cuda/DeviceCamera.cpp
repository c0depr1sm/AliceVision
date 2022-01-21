// This file is part of the AliceVision project.
// Copyright (c) 2022 AliceVision contributors.
// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "DeviceCamera.hpp"

#include <aliceVision/depthMap/cuda/imageProcessing/deviceGaussianFilter.hpp>
#include <aliceVision/depthMap/cuda/imageProcessing/deviceColorConversion.hpp>

namespace aliceVision {
namespace depthMap {

void buildFrameCudaTexture(CudaDeviceMemoryPitched<CudaRGBA, 2>* frame_dmp, cudaTextureObject_t* textureObject)
{
    cudaTextureDesc texDesc;
    memset(&texDesc, 0, sizeof(cudaTextureDesc));
    texDesc.normalizedCoords = 0; // addressed (x,y) in [width,height]
    texDesc.addressMode[0] = cudaAddressModeClamp;
    texDesc.addressMode[1] = cudaAddressModeClamp;
    texDesc.addressMode[2] = cudaAddressModeClamp;

#if defined(ALICEVISION_DEPTHMAP_TEXTURE_USE_UCHAR) && defined(ALICEVISION_DEPTHMAP_TEXTURE_USE_INTERPOLATION)
    tex_desc.readMode = cudaReadModeNormalizedFloat; // uchar to float [0:1], see tex2d_float4 function
#else
    texDesc.readMode = cudaReadModeElementType;
#endif

#ifdef ALICEVISION_DEPTHMAP_TEXTURE_USE_INTERPOLATION
    // with subpixel interpolation (can have a large performance impact on some graphic cards)
    // but could be critical for quality during SGM in small resolution
    texDesc.filterMode = cudaFilterModeLinear;
#else
    // without interpolation
    tex_desc.filterMode = cudaFilterModePoint;
#endif

    cudaResourceDesc resDesc;
    resDesc.resType = cudaResourceTypePitch2D;
    resDesc.res.pitch2D.desc = cudaCreateChannelDesc<CudaRGBA>();
    resDesc.res.pitch2D.devPtr = frame_dmp->getBuffer();
    resDesc.res.pitch2D.width = frame_dmp->getSize()[0];
    resDesc.res.pitch2D.height = frame_dmp->getSize()[1];
    resDesc.res.pitch2D.pitchInBytes = frame_dmp->getPitch();

    cudaError_t err = cudaCreateTextureObject(textureObject, &resDesc, &texDesc, 0);
    THROW_ON_CUDA_ERROR(err, "Failed to bind texture object to camera frame array");
}

DeviceCamera::DeviceCamera(int deviceCamId)
    : _deviceCamId(deviceCamId)
    , _globalCamId(-1)
    , _originalWidth(-1)
    , _originalHeight(-1)
    , _width(-1)
    , _height(-1)
    , _downscale(-1)
    , _memBytes(0)
{}

DeviceCamera::~DeviceCamera()
{
    _frame_dmp.reset();
    cudaFreeHost(_cameraParameters_h);
    cudaDestroyTextureObject(_textureObject);
}

void DeviceCamera::fill(int globalCamId, 
                        int downscale, 
                        int originalWidth, 
                        int originalHeight, 
                        const CudaHostMemoryHeap<CudaRGBA, 2>& frame_hmh,
                        const DeviceCameraParams& cameraParameters_h, 
                        cudaStream_t stream)
{
    // update members
    _globalCamId = globalCamId;
    _originalWidth = originalWidth;
    _originalHeight = originalHeight;
    _width = _originalWidth / downscale;
    _height = _originalHeight / downscale;
    _downscale = downscale;

    // allocate or re-allocate the host-sided camera params
    {
        cudaFreeHost(_cameraParameters_h);
        cudaError_t err = cudaMallocHost(&_cameraParameters_h, sizeof(DeviceCameraParams));
        THROW_ON_CUDA_ERROR(err, "Could not allocate camera parameters in pinned host memory in " << __FILE__ << ":" << __LINE__ << ", " << cudaGetErrorString(err));
    }
    
    // copy the given camera parameters
    *_cameraParameters_h = cameraParameters_h;

    // copy the host-sided camera params in device constant camera params array
    {
        cudaMemcpyKind kind = cudaMemcpyHostToDevice;
        cudaError_t err;

        if(stream == 0)
        {
            err = cudaMemcpyToSymbol(constantCameraParametersArray_d, _cameraParameters_h, sizeof(DeviceCameraParams),
                                     _deviceCamId * sizeof(DeviceCameraParams), kind);
        }
        else
        {
            err = cudaMemcpyToSymbolAsync(constantCameraParametersArray_d, _cameraParameters_h, sizeof(DeviceCameraParams),
                                          _deviceCamId * sizeof(DeviceCameraParams), kind, stream);
        }

        THROW_ON_CUDA_ERROR(err, "Failed to copy DeviceCameraParams from host to device in " << __FILE__ << ":" << __LINE__ << ": " << cudaGetErrorString(err));
    }

    // allocate or re-allocate device frame if needed
    const CudaSize<2> deviceFrameSize(_width, _height);

    if(_frame_dmp.get() == nullptr || _frame_dmp->getSize() != deviceFrameSize)
    {
        // allocate or re-allocate the device-sided data buffer with the new size
        _frame_dmp.reset(new CudaDeviceMemoryPitched<CudaRGBA, 2>(deviceFrameSize));
        _memBytes = _frame_dmp->getBytesPadded();

        // re-build the associated CUDA texture object
        cudaDestroyTextureObject(_textureObject);
        buildFrameCudaTexture(_frame_dmp.get(), &_textureObject);
    }

    // update device frame
    fillDeviceFrameFromHostFrame(frame_hmh, stream);
}

void DeviceCamera::fillDeviceFrameFromHostFrame(const CudaHostMemoryHeap<CudaRGBA, 2>& frame_hmh, cudaStream_t stream)
{
    if(_downscale <= 1)
    {
        // no need to downscale
        assert(_originalHeight == _height);
        assert(_originalWidth == _width);

        // copy texture's data from host to device
        _frame_dmp->copyFrom(frame_hmh, stream);
    }
    else
    {
        // allocate the full size device-sided data buffer and build the texture object
        CudaDeviceMemoryPitched<CudaRGBA, 2>* deviceFrameToDownscale = new CudaDeviceMemoryPitched<CudaRGBA, 2>(frame_hmh.getSize());
        cudaTextureObject_t textureObjectToDownscale;
        buildFrameCudaTexture(deviceFrameToDownscale, &textureObjectToDownscale);

        // copy the full size host-sided data buffer onto the device-sided data buffer
        deviceFrameToDownscale->copyFrom(frame_hmh, stream);

        // downscale with gaussian blur the initial texture 
        const int gaussianFilterRadius = _downscale;
        cuda_downscaleWithGaussianBlur(*_frame_dmp, textureObjectToDownscale, _downscale, _width, _height, gaussianFilterRadius, stream);

        // delete full size data buffer on the GPU.
        delete deviceFrameToDownscale;
        cudaDestroyTextureObject(textureObjectToDownscale);
    }

    // in-place color conversion into CIELAB
    cuda_rgb2lab(*_frame_dmp, _width, _height, stream);
}

} // namespace depthMap
} // namespace aliceVision
