/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/jit/xla_platform_info.h"

#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "tensorflow/compiler/jit/device_executable_persistor.h"
#include "tensorflow/compiler/jit/flags.h"
#include "tensorflow/compiler/jit/xla_device_compiler_client.h"
#include "tensorflow/compiler/xla/client/client_library.h"
#include "tensorflow/compiler/xla/client/local_client.h"
#include "tensorflow/core/tpu/tpu_defs.h"

namespace tensorflow {
namespace {
using XlaDeviceCompiler =
    DeviceCompiler<xla::LocalExecutable, xla::LocalClient>;
using XlaDeviceExecutablePersistor =
    DeviceExecutablePersistor<xla::LocalExecutable, xla::LocalClient>;

XlaDeviceCompiler* CreateXlaDeviceCompiler(
    const XlaDeviceExecutablePersistor::Config& persistor_config,
    DeviceType device_type, xla::LocalClient* local_client) {
  auto persistor = std::make_unique<XlaDeviceExecutablePersistor>(
      std::move(persistor_config), device_type);
  auto compiler_client =
      std::make_unique<XlaDeviceCompilerClient>(local_client);
  return new XlaDeviceCompiler(std::move(persistor),
                               std::move(compiler_client));
}

StatusOr<std::optional<std::set<int>>> GetAllowedGpus(
    FunctionLibraryRuntime* flr) {
  std::optional<std::set<int>> gpu_ids = std::nullopt;

  if (flr->config_proto()) {
    string allowed_gpus =
        flr->config_proto()->gpu_options().visible_device_list();
    TF_ASSIGN_OR_RETURN(gpu_ids, ParseVisibleDeviceList(allowed_gpus));
  }

  return gpu_ids;
}
}  // namespace

xla::StatusOr<std::optional<std::set<int>>> ParseVisibleDeviceList(
    absl::string_view visible_device_list) {
  std::set<int> gpu_ids;
  if (visible_device_list.empty()) {
    return {{std::nullopt}};
  }
  const std::vector<string> visible_devices =
      absl::StrSplit(visible_device_list, ',');
  for (const string& platform_device_id_str : visible_devices) {
    int32_t platform_device_id;
    if (!absl::SimpleAtoi(platform_device_id_str, &platform_device_id)) {
      return errors::InvalidArgument(
          "Could not parse entry in 'visible_device_list': '",
          platform_device_id_str,
          "'. visible_device_list = ", visible_device_list);
    }
    gpu_ids.insert(platform_device_id);
  }
  return {{gpu_ids}};
}

Status BuildXlaDeviceCompiler(DeviceBase* device, FunctionLibraryRuntime* flr,
                              const XlaPlatformInfo& platform_info,
                              XlaDeviceCompiler** xla_device_compiler) {
  XlaDeviceExecutablePersistor::Config persistor_config(
      GetMarkForCompilationPassFlags()->tf_xla_persistent_cache_directory,
      GetMarkForCompilationPassFlags()->tf_xla_disable_strict_signature_checks,
      GetMarkForCompilationPassFlags()->tf_xla_persistent_cache_prefix);

  if (platform_info.xla_device_metadata()) {
    *xla_device_compiler = CreateXlaDeviceCompiler(
        persistor_config,
        platform_info.xla_device_metadata()->jit_device_type(),
        platform_info.xla_device_metadata()->client());
    return OkStatus();
  }

  // TFRT-TPU is used if device type is `DEVICE_TPU` and platform_info does not
  // have `xla_device_metadata`.
  if (platform_info.device_type() == DEVICE_TPU) {
    *xla_device_compiler = CreateXlaDeviceCompiler(
        persistor_config, DeviceType(DEVICE_TPU_XLA_JIT), nullptr);
    return OkStatus();
  }

  auto platform =
      se::MultiPlatformManager::PlatformWithId(platform_info.platform_id());
  if (!platform.ok()) {
    return platform.status();
  }

  StatusOr<xla::Compiler*> compiler_for_platform =
      xla::Compiler::GetForPlatform(platform.value());
  if (!compiler_for_platform.ok()) {
    // In some rare cases (usually in unit tests with very small clusters) we
    // may end up transforming an XLA cluster with at least one GPU operation
    // (which would normally force the cluster to be compiled using XLA:GPU)
    // into an XLA cluster with no GPU operations (i.e. containing only CPU
    // operations).  Such a cluster can fail compilation (in way that
    // MarkForCompilation could not have detected) if the CPU JIT is not linked
    // in.
    //
    // So bail out of _XlaCompile in this case, and let the executor handle the
    // situation for us.
    const Status& status = compiler_for_platform.status();
    if (status.code() == error::NOT_FOUND) {
      return errors::Unimplemented("Could not find compiler for platform ",
                                   platform.value()->Name(), ": ",
                                   status.ToString());
    }
  }

  xla::LocalClientOptions client_options;
  client_options.set_platform(platform.value());
  client_options.set_intra_op_parallelism_threads(
      device->tensorflow_cpu_worker_threads()->num_threads);

  TF_ASSIGN_OR_RETURN(auto allowed_gpus, GetAllowedGpus(flr));
  client_options.set_allowed_devices(allowed_gpus);

  auto client = xla::ClientLibrary::GetOrCreateLocalClient(client_options);
  if (!client.ok()) {
    return client.status();
  }
  const XlaOpRegistry::DeviceRegistration* registration;
  if (!XlaOpRegistry::GetCompilationDevice(platform_info.device_type().type(),
                                           &registration)) {
    return errors::InvalidArgument("No JIT device registered for ",
                                   platform_info.device_type().type());
  }

  *xla_device_compiler = CreateXlaDeviceCompiler(
      persistor_config, DeviceType(registration->compilation_device_name),
      client.value());
  return OkStatus();
}

XlaPlatformInfo XlaPlatformInfoFromDevice(DeviceBase* device_base) {
  auto device = static_cast<Device*>(device_base);
  se::Platform::Id platform_id = nullptr;
  const XlaDevice::Metadata* xla_device_metadata = nullptr;
  std::shared_ptr<se::DeviceMemoryAllocator> custom_allocator;

  if (device->device_type() == DEVICE_CPU) {
    platform_id = se::host::kHostPlatformId;
  } else if (device->device_type() == DEVICE_GPU) {
    platform_id = device->tensorflow_accelerator_device_info()
                      ->stream->parent()
                      ->platform()
                      ->id();
  } else if (XlaDevice::GetMetadataFromDevice(device, &xla_device_metadata)
                 .ok()) {
    // If we are on an XlaDevice, use the underlying XLA platform's allocator
    // directly. We could use the StreamExecutor's allocator which may
    // theoretically be more correct, but XLA returns a nice OOM message in a
    // Status and StreamExecutor does not.
    //
    // Importantly we can't use ctx->device()->GetAllocator() as the allocator
    // (which xla_allocator above uses) as on an XlaDevice, this is a dummy
    // allocator that returns XlaTensor objects. The XlaCompiler needs a real
    // allocator to allocate real buffers.
    platform_id = xla_device_metadata->platform()->id();
    custom_allocator =
        xla_device_metadata->client()->backend().shared_memory_allocator();
  }

  return XlaPlatformInfo(DeviceType(device->device_type()), platform_id,
                         xla_device_metadata, custom_allocator);
}

std::shared_ptr<se::DeviceMemoryAllocator> GetAllocator(
    DeviceBase* device, se::Stream* stream,
    const XlaPlatformInfo& platform_info) {
  if (platform_info.custom_allocator()) {
    return platform_info.custom_allocator();
  }
  auto* alloc = device->GetAllocator({});
  if (!stream) {
    // Stream is not set for the host platform.
    se::Platform* platform =
        se::MultiPlatformManager::PlatformWithId(platform_info.platform_id())
            .value();
    return std::make_shared<se::TfAllocatorAdapter>(alloc, platform);
  }
  return std::make_shared<se::TfAllocatorAdapter>(alloc, stream);
}

}  // namespace tensorflow
