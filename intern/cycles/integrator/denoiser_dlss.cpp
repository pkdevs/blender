/* SPDX-FileCopyrightText: 2025 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_DLSS

#  include "integrator/denoiser_dlss.h"
#  include "integrator/pass_accessor_gpu.h"

#  include "device/cuda/device_impl.h"

#  include "util/path.h"

#  include <nvsdk_ngx.h>
#  include <nvsdk_ngx_defs_dlssd.h>

#  ifdef _WIN32
#    include "util/windows.h"

#    define dynamic_library_open(path) LoadLibraryW(path)
#    define dynamic_library_close(lib) FreeLibrary(static_cast<HMODULE>(lib))
#    define dynamic_library_find(lib, symbol) \
      reinterpret_cast<t##symbol>(GetProcAddress(static_cast<HMODULE>(lib), #symbol))
#  else
#    include <dlfcn.h>

#    define dynamic_library_open(path) dlopen(path, RTLD_NOW)
#    define dynamic_library_close(lib) dlclose(lib)
#    define dynamic_library_find(lib, symbol) reinterpret_cast<t##symbol>(dlsym(lib, #symbol))
#  endif

struct NGXDriver {
  using tNVSDK_NGX_CUDA_Init_Ext1 = NVSDK_NGX_Result (*)(unsigned long long,
                                                         const wchar_t *,
                                                         NVSDK_NGX_CUDADevice *,
                                                         NVSDK_NGX_Version,
                                                         const NVSDK_NGX_FeatureCommonInfo *);
  using tNVSDK_NGX_CUDA_Shutdown1 = NVSDK_NGX_Result (*)(NVSDK_NGX_CUDADevice *, unsigned int &);
  using tNVSDK_NGX_CUDA_GetFeatureRequirements = decltype(&NVSDK_NGX_CUDA_GetFeatureRequirements);
  using tNVSDK_NGX_CUDA_CreateFeature1 = decltype(&NVSDK_NGX_CUDA_CreateFeature1);
  using tNVSDK_NGX_CUDA_EvaluateFeature = decltype(&NVSDK_NGX_CUDA_EvaluateFeature);
  using tNVSDK_NGX_CUDA_ReleaseFeature = decltype(&NVSDK_NGX_CUDA_ReleaseFeature);
  using tNVSDK_NGX_CUDA_AllocateParameters = decltype(&NVSDK_NGX_CUDA_AllocateParameters);
  using tNVSDK_NGX_CUDA_DestroyParameters = decltype(&NVSDK_NGX_CUDA_DestroyParameters);

  tNVSDK_NGX_CUDA_Init_Ext1 Init_Ext1 = nullptr;
  tNVSDK_NGX_CUDA_Shutdown1 Shutdown1 = nullptr;
  tNVSDK_NGX_CUDA_GetFeatureRequirements GetFeatureRequirements = nullptr;
  tNVSDK_NGX_CUDA_CreateFeature1 CreateFeature1 = nullptr;
  tNVSDK_NGX_CUDA_EvaluateFeature EvaluateFeature = nullptr;
  tNVSDK_NGX_CUDA_ReleaseFeature ReleaseFeature = nullptr;
  tNVSDK_NGX_CUDA_AllocateParameters AllocateParameters = nullptr;
  tNVSDK_NGX_CUDA_DestroyParameters DestroyParameters = nullptr;

  explicit operator bool() const
  {
    return Init_Ext1 != nullptr && Shutdown1 != nullptr && CreateFeature1 != nullptr &&
           EvaluateFeature != nullptr && ReleaseFeature != nullptr &&
           AllocateParameters != nullptr && DestroyParameters != nullptr;
  }

  bool init()
  {
    if (*this) {
      return true;
    }

#  ifdef _WIN32
    WCHAR ngx_path[MAX_PATH] = L"";
    {
      HKEY ngx_key = nullptr;
      LSTATUS result = RegOpenKeyExW(
          HKEY_LOCAL_MACHINE,
          L"System\\CurrentControlSet\\Services\\nvlddmkm\\Parameters\\NGXCore",
          0,
          KEY_READ,
          &ngx_key);
      if (result != ERROR_SUCCESS) {
        result = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                               L"System\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore",
                               0,
                               KEY_READ,
                               &ngx_key);
      }
      if (result == ERROR_SUCCESS) {
        DWORD ngx_path_size = ARRAYSIZE(ngx_path);
        result = RegQueryValueExW(
            ngx_key, L"NGXPath", 0, nullptr, reinterpret_cast<LPBYTE>(ngx_path), &ngx_path_size);
        RegCloseKey(ngx_key);
      }
      if (result != ERROR_SUCCESS) {
        return false;
      }

      wcscat_s(ngx_path, L"\\_nvngx.dll");
    }
#  else
    const char *const ngx_path = "libnvidia-ngx.so.1";
#  endif

    void *const ngx_module = dynamic_library_open(ngx_path);
    if (ngx_module == nullptr) {
      return false;
    }

    Init_Ext1 = dynamic_library_find(ngx_module, NVSDK_NGX_CUDA_Init_Ext1);
    Shutdown1 = dynamic_library_find(ngx_module, NVSDK_NGX_CUDA_Shutdown1);
    GetFeatureRequirements = dynamic_library_find(ngx_module,
                                                  NVSDK_NGX_CUDA_GetFeatureRequirements);
    CreateFeature1 = dynamic_library_find(ngx_module, NVSDK_NGX_CUDA_CreateFeature1);
    EvaluateFeature = dynamic_library_find(ngx_module, NVSDK_NGX_CUDA_EvaluateFeature);
    ReleaseFeature = dynamic_library_find(ngx_module, NVSDK_NGX_CUDA_ReleaseFeature);
    AllocateParameters = dynamic_library_find(ngx_module, NVSDK_NGX_CUDA_AllocateParameters);
    DestroyParameters = dynamic_library_find(ngx_module, NVSDK_NGX_CUDA_DestroyParameters);

    if (*this) {
      return true;
    }
    else {
      dynamic_library_close(ngx_module);
      return false;
    }
  }
} NVSDK_NGX_CUDA;

CCL_NAMESPACE_BEGIN

static const int ApplicationId = 100334311;

void DLSSDenoiser::CUDATexture::init(Device *device, int width, int height, int num_components)
{
  CUDA_ARRAY_DESCRIPTOR desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.Format = CU_AD_FORMAT_FLOAT;
  desc.NumChannels = num_components;

  cuda_device_assert(device, cuArrayCreate((CUarray *)&array, &desc));

  CUDA_TEXTURE_DESC tex_desc = {};
  tex_desc.addressMode[0] = CU_TR_ADDRESS_MODE_CLAMP;
  tex_desc.addressMode[1] = CU_TR_ADDRESS_MODE_CLAMP;
  tex_desc.addressMode[2] = CU_TR_ADDRESS_MODE_CLAMP;
  tex_desc.flags = CU_TRSF_NORMALIZED_COORDINATES;

  CUDA_RESOURCE_DESC res_desc = {};
  res_desc.resType = CU_RESOURCE_TYPE_ARRAY;
  res_desc.res.array.hArray = (CUarray)array;

  cuda_device_assert(
      device, cuTexObjectCreate((CUtexObject *)&texture_handle, &res_desc, &tex_desc, nullptr));
  cuda_device_assert(device, cuSurfObjectCreate((CUsurfObject *)&surface_handle, &res_desc));
}
void DLSSDenoiser::CUDATexture::destroy()
{
  cuSurfObjectDestroy((CUsurfObject)surface_handle);
  surface_handle = 0;
  cuTexObjectDestroy((CUtexObject)texture_handle);
  texture_handle = 0;

  cuArrayDestroy((CUarray)array);
  array = 0;
}

DLSSDenoiser::DLSSDenoiser(Device *denoiser_device, const DenoiseParams &params)
    : DenoiserGPU(denoiser_device, params)
{
  CUDADevice *const cuda_device = static_cast<CUDADevice *>(denoiser_device_);
  const CUDAContextScope scope(cuda_device);

  if (!NVSDK_NGX_CUDA.init()) {
    set_error("Failed to load NGX driver");
    return;
  }

#  ifdef _WIN32
  const wstring app_path = string_to_wstring(path_get());
  const wstring user_path = string_to_wstring(path_user_get());
#  else
  string app_path_narrow = path_get();
  string user_path_narrow = path_user_get();
  std::wstring app_path(app_path_narrow.size(), L' ');
  std::wstring user_path(user_path_narrow.size(), L' ');
  app_path.resize(std::mbstowcs(app_path.data(), app_path_narrow.c_str(), app_path_narrow.size()));
  user_path.resize(
      std::mbstowcs(user_path.data(), user_path_narrow.c_str(), user_path_narrow.size()));
#  endif

  const wchar_t *const app_paths[] = {app_path.c_str()};

  NVSDK_NGX_FeatureCommonInfo feature_info = {};
  feature_info.PathListInfo.Path = app_paths;
  feature_info.PathListInfo.Length = 1;
  feature_info.LoggingInfo.LoggingCallback =
      [](const char *message, NVSDK_NGX_Logging_Level loggingLevel, NVSDK_NGX_Feature) {
        switch (loggingLevel) {
          case NVSDK_NGX_LOGGING_LEVEL_OFF:
          case NVSDK_NGX_LOGGING_LEVEL_NUM:
            assert(false);
            break;
          case NVSDK_NGX_LOGGING_LEVEL_ON:
            LOG_INFO << message;
            break;
          case NVSDK_NGX_LOGGING_LEVEL_VERBOSE:
            LOG_INFO << message;
            break;
        }
      };
  feature_info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;

  ngx_device_ = new NVSDK_NGX_CUDADevice{
      cuda_device->cuContext, static_cast<CUDADeviceQueue *>(denoiser_queue_.get())->stream()};

  const NVSDK_NGX_Result result = NVSDK_NGX_CUDA.Init_Ext1(
      ApplicationId, user_path.c_str(), ngx_device_, NVSDK_NGX_Version_API, &feature_info);

  if (NVSDK_NGX_FAILED(result)) {
    set_error("Failed to initialize NGX driver");
  }
}

DLSSDenoiser::~DLSSDenoiser()
{
  CUDADevice *const cuda_device = static_cast<CUDADevice *>(denoiser_device_);
  const CUDAContextScope scope(cuda_device);

  tex_color_.destroy();
  tex_depth_.destroy();
  tex_diffuse_albedo_.destroy();
  tex_specular_albedo_.destroy();
  tex_normal_roughness_.destroy();
  tex_motion_.destroy();
  tex_specular_motion_.destroy();
  tex_output_.destroy();

  if (!NVSDK_NGX_CUDA) {
    return;
  }

  if (handle_ != nullptr) {
    NVSDK_NGX_CUDA.ReleaseFeature(handle_);
  }

  unsigned int n = 0;
  const NVSDK_NGX_Result result = NVSDK_NGX_CUDA.Shutdown1(ngx_device_, n);

  if (NVSDK_NGX_FAILED(result)) {
    set_error("Failed to shutdown NGX driver");
  }

  delete ngx_device_;
}

bool DLSSDenoiser::is_device_supported(const DeviceInfo &device)
{
  if (device.type != DEVICE_CUDA && device.type != DEVICE_OPTIX) {
    return false;
  }

  /* 'NVSDK_NGX_CUDA_GetFeatureRequirements' is an expensive call, so cache the result (since
   * 'is_device_supported' is called a lot). */
  static NVSDK_NGX_Feature_Support_Result supported_cache[8] = {
      NVSDK_NGX_FeatureSupportResult_CheckNotPresent,
      NVSDK_NGX_FeatureSupportResult_CheckNotPresent,
      NVSDK_NGX_FeatureSupportResult_CheckNotPresent,
      NVSDK_NGX_FeatureSupportResult_CheckNotPresent,
      NVSDK_NGX_FeatureSupportResult_CheckNotPresent,
      NVSDK_NGX_FeatureSupportResult_CheckNotPresent,
      NVSDK_NGX_FeatureSupportResult_CheckNotPresent,
      NVSDK_NGX_FeatureSupportResult_CheckNotPresent};

  if (device.num >= 8) {
    return false;
  }
  if (supported_cache[device.num] != NVSDK_NGX_FeatureSupportResult_CheckNotPresent) {
    return supported_cache[device.num] == NVSDK_NGX_FeatureSupportResult_Supported;
  }

  if (!NVSDK_NGX_CUDA.init() || NVSDK_NGX_CUDA.GetFeatureRequirements == nullptr) {
    /* NVIDIA driver is too old (requires 590+). */
    return false;
  }

#  ifdef _WIN32
  const wstring user_path = string_to_wstring(path_user_get());
#  else
  string user_path_narrow = path_user_get();
  std::wstring user_path(user_path_narrow.size(), L' ');
  user_path.resize(
      std::mbstowcs(user_path.data(), user_path_narrow.c_str(), user_path_narrow.size()));
#  endif

  CUdevice cuDevice = 0;
  cuDeviceGet(&cuDevice, device.num);

  NVSDK_NGX_FeatureDiscoveryInfo discovery_info = {};
  discovery_info.SDKVersion = NVSDK_NGX_Version_API;
  discovery_info.FeatureID = NVSDK_NGX_Feature_RayReconstruction;
  discovery_info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Application_Id;
  discovery_info.Identifier.v.ApplicationId = ApplicationId;
  discovery_info.ApplicationDataPath = user_path.c_str();

  NVSDK_NGX_FeatureRequirement requirement = {NVSDK_NGX_FeatureSupportResult_Supported};

  const NVSDK_NGX_Result result = NVSDK_NGX_CUDA.GetFeatureRequirements(
      cuDevice, &discovery_info, &requirement);

  if (NVSDK_NGX_SUCCEED(result)) {
    supported_cache[device.num] = requirement.FeatureSupported;
    return requirement.FeatureSupported == NVSDK_NGX_FeatureSupportResult_Supported;
  }
  else {
    return false;
  }
}

bool DLSSDenoiser::denoise_create_if_needed(DenoiseContext &context)
{
  const bool recreate_denoiser = last_width_ != context.denoised_buffer_params.width ||
                                 last_height_ != context.denoised_buffer_params.height ||
                                 last_upscale_factor_ != context.denoise_params.upscale_factor;
  if (handle_ != nullptr && !recreate_denoiser) {
    return true;
  }

  CUDADevice *const cuda_device = static_cast<CUDADevice *>(denoiser_device_);
  const CUDAContextScope scope(cuda_device);

  if (handle_ != nullptr) {
    denoiser_queue_->synchronize();

    NVSDK_NGX_CUDA.ReleaseFeature(handle_);
    handle_ = nullptr;
  }

  tex_color_.destroy();
  tex_depth_.destroy();
  tex_diffuse_albedo_.destroy();
  tex_specular_albedo_.destroy();
  tex_normal_roughness_.destroy();
  tex_motion_.destroy();
  tex_specular_motion_.destroy();
  tex_output_.destroy();

  if (context.buffer_params.width <= 128 || context.buffer_params.height <= 96) {
    last_width_ = 0;
    last_height_ = 0;
    return false;
  }

  NVSDK_NGX_Parameter *params = nullptr;
  if (NVSDK_NGX_FAILED(NVSDK_NGX_CUDA.AllocateParameters(&params))) {
    return false;
  }

  params->Set(NVSDK_NGX_Parameter_Width, context.buffer_params.width);
  params->Set(NVSDK_NGX_Parameter_Height, context.buffer_params.height);
  params->Set(NVSDK_NGX_Parameter_OutWidth, context.denoised_buffer_params.width);
  params->Set(NVSDK_NGX_Parameter_OutHeight, context.denoised_buffer_params.height);

  params->Set(NVSDK_NGX_Parameter_DLSS_Denoise_Mode, NVSDK_NGX_DLSS_Denoise_Mode_DLUnified);
  params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
              NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes);
  params->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
  params->Set(NVSDK_NGX_Parameter_PerfQualityValue,
              context.denoise_params.upscale_factor == 1.0f ?
                  NVSDK_NGX_PerfQuality_Value_DLAA :
              context.denoise_params.upscale_factor <= 1.0f / 0.65f ?
                  NVSDK_NGX_PerfQuality_Value_MaxQuality :
              context.denoise_params.upscale_factor <= 1.0f / 0.57f ?
                  NVSDK_NGX_PerfQuality_Value_Balanced :
              context.denoise_params.upscale_factor <= 1.0f / 0.5f ?
                  NVSDK_NGX_PerfQuality_Value_MaxPerf :
                  NVSDK_NGX_PerfQuality_Value_UltraPerformance);
  params->Set(NVSDK_NGX_Parameter_Use_HW_Depth, NVSDK_NGX_DLSS_Depth_Type_Linear);
  params->Set(NVSDK_NGX_Parameter_DLSS_Roughness_Mode, NVSDK_NGX_DLSS_Roughness_Mode_Packed);

  params->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA,
              NVSDK_NGX_RayReconstruction_Hint_Render_Preset_E);
  params->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality,
              NVSDK_NGX_RayReconstruction_Hint_Render_Preset_E);
  params->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced,
              NVSDK_NGX_RayReconstruction_Hint_Render_Preset_E);
  params->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance,
              NVSDK_NGX_RayReconstruction_Hint_Render_Preset_E);
  params->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance,
              NVSDK_NGX_RayReconstruction_Hint_Render_Preset_E);

  const NVSDK_NGX_Result result = NVSDK_NGX_CUDA.CreateFeature1(
      ngx_device_, NVSDK_NGX_Feature_RayReconstruction, params, &handle_);

  NVSDK_NGX_CUDA.DestroyParameters(params);

  if (NVSDK_NGX_FAILED(result)) {
    set_error("Failed to create DLSS instance");
    return false;
  }

  tex_color_.init(cuda_device, context.buffer_params.width, context.buffer_params.height, 4);
  tex_depth_.init(cuda_device, context.buffer_params.width, context.buffer_params.height, 1);
  tex_diffuse_albedo_.init(
      cuda_device, context.buffer_params.width, context.buffer_params.height, 4);
  tex_specular_albedo_.init(
      cuda_device, context.buffer_params.width, context.buffer_params.height, 4);
  tex_normal_roughness_.init(
      cuda_device, context.buffer_params.width, context.buffer_params.height, 4);
  tex_motion_.init(cuda_device, context.buffer_params.width, context.buffer_params.height, 2);
  tex_specular_motion_.init(
      cuda_device, context.buffer_params.width, context.buffer_params.height, 2);

  tex_output_.init(
      cuda_device, context.denoised_buffer_params.width, context.denoised_buffer_params.height, 4);

  last_width_ = context.denoised_buffer_params.width;
  last_height_ = context.denoised_buffer_params.height;
  last_upscale_factor_ = context.denoise_params.upscale_factor;

  return !cuda_device->have_error();
}

bool DLSSDenoiser::denoise_configure_if_needed(DenoiseContext & /*context*/)
{
  return true;
}

bool DLSSDenoiser::denoise_filter_color_preprocess(const DenoiseContext &context,
                                                   const DenoisePass &pass)
{
  if (pass.type != PASS_COMBINED) {
    return true;
  }

  // Input params (with resolution divider applied)
  const BufferParams &buffer_params = context.buffer_params;

  const int work_size = buffer_params.width * buffer_params.height;

  const DeviceKernelArguments args(&tex_color_.surface_handle,
                                   &context.render_buffers->buffer.device_pointer,
                                   &buffer_params.full_x,
                                   &buffer_params.full_y,
                                   &buffer_params.width,
                                   &buffer_params.height,
                                   &buffer_params.offset,
                                   &buffer_params.stride,
                                   &buffer_params.pass_stride,
                                   &pass.denoised_offset);

  return denoiser_queue_->enqueue(
      DEVICE_KERNEL_FILTER_COLOR_PREPROCESS_TO_SURFACE, work_size, args);
}
bool DLSSDenoiser::denoise_filter_color_postprocess(const DenoiseContext &context,
                                                    const DenoisePass &pass)
{
  if (pass.type != PASS_COMBINED) {
    return true;
  }

  // Output params
  const BufferParams &buffer_params = context.denoised_buffer_params;

  const int work_size = buffer_params.width * buffer_params.height;

  const DeviceKernelArguments args(&tex_output_.surface_handle,
                                   &context.render_buffers->buffer.device_pointer,
                                   &buffer_params.full_x,
                                   &buffer_params.full_y,
                                   &buffer_params.width,
                                   &buffer_params.height,
                                   &buffer_params.offset,
                                   &buffer_params.stride,
                                   &context.buffer_params.full_x,
                                   &context.buffer_params.full_y,
                                   &context.buffer_params.offset,
                                   &context.buffer_params.stride,
                                   &buffer_params.pass_stride,
                                   &context.num_samples,
                                   &pass.noisy_offset,
                                   &pass.denoised_offset,
                                   &context.pass_sample_count,
                                   &pass.num_components,
                                   &pass.use_compositing,
                                   &params_.upscale_factor);

  return denoiser_queue_->enqueue(
      DEVICE_KERNEL_FILTER_COLOR_POSTPROCESS_FROM_SURFACE, work_size, args);
}

bool DLSSDenoiser::denoise_filter_guiding_preprocess(DenoiseContext &context)
{
  const BufferParams &buffer_params = context.buffer_params;

  const int work_size = buffer_params.width * buffer_params.height;

  const int pass_depth = context.buffer_params.get_pass_offset(PASS_DENOISING_DEPTH);
  const int pass_specular_albedo = context.buffer_params.get_pass_offset(
      PASS_DENOISING_SPECULAR_ALBEDO);
  const int pass_roughness = context.buffer_params.get_pass_offset(PASS_DENOISING_ROUGHNESS);
  const int pass_specular_motion = context.buffer_params.get_pass_offset(
      PASS_DENOISING_SPECULAR_MOTION);

  const DeviceKernelArguments args(&tex_depth_.surface_handle,
                                   &tex_diffuse_albedo_.surface_handle,
                                   &tex_specular_albedo_.surface_handle,
                                   &tex_normal_roughness_.surface_handle,
                                   &tex_motion_.surface_handle,
                                   &tex_specular_motion_.surface_handle,
                                   &context.render_buffers->buffer.device_pointer,
                                   &buffer_params.offset,
                                   &buffer_params.stride,
                                   &buffer_params.pass_stride,
                                   &context.pass_sample_count,
                                   &pass_depth,
                                   &context.pass_denoising_albedo,
                                   &pass_specular_albedo,
                                   &context.pass_denoising_normal,
                                   &pass_roughness,
                                   &context.pass_motion,
                                   &pass_specular_motion,
                                   &buffer_params.full_x,
                                   &buffer_params.full_y,
                                   &buffer_params.width,
                                   &buffer_params.height,
                                   &context.num_samples);

  return denoiser_queue_->enqueue(
      DEVICE_KERNEL_FILTER_GUIDING_PREPROCESS_TO_SURFACE, work_size, args);
}

bool DLSSDenoiser::denoise_run(const DenoiseContext &context, const DenoisePass &pass)
{
  if (pass.type != PASS_COMBINED) {
    return true;
  }

  NVSDK_NGX_Parameter *params = nullptr;
  if (NVSDK_NGX_FAILED(NVSDK_NGX_CUDA.AllocateParameters(&params))) {
    return false;
  }

  CUDADevice *const cuda_device = static_cast<CUDADevice *>(denoiser_device_);
  const CUDAContextScope scope(cuda_device);

  params->Set(NVSDK_NGX_Parameter_Reset, 0);

  params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, context.pixel_jitter.x);
  params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, context.pixel_jitter.y);

  params->Set(NVSDK_NGX_Parameter_Color, &tex_color_.texture_handle);
  params->Set(NVSDK_NGX_Parameter_Depth, &tex_depth_.texture_handle);
  params->Set(NVSDK_NGX_Parameter_DiffuseAlbedo, &tex_diffuse_albedo_.texture_handle);
  params->Set(NVSDK_NGX_Parameter_SpecularAlbedo, &tex_specular_albedo_.texture_handle);
  params->Set(NVSDK_NGX_Parameter_GBuffer_Normals, &tex_normal_roughness_.texture_handle);
  params->Set(NVSDK_NGX_Parameter_GBuffer_Roughness, &tex_normal_roughness_.texture_handle);
  params->Set(NVSDK_NGX_Parameter_MotionVectors, &tex_motion_.texture_handle);
  params->Set(NVSDK_NGX_Parameter_GBuffer_SpecularMvec, &tex_specular_motion_.texture_handle);
  params->Set(NVSDK_NGX_Parameter_Output, &tex_output_.surface_handle);

  params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
              context.buffer_params.width);
  params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,
              context.buffer_params.height);

  params->Set(NVSDK_NGX_Parameter_DLSS_Indicator_Invert_Y_Axis, 1);

  const NVSDK_NGX_Result result = NVSDK_NGX_CUDA.EvaluateFeature(handle_, params, nullptr);

  NVSDK_NGX_CUDA.DestroyParameters(params);

  return NVSDK_NGX_SUCCEED(result);
}

CCL_NAMESPACE_END

#endif
