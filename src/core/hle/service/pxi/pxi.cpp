// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cinttypes>
#include "core/hle/service/pxi/pxi.h"
#include "core/hle/service/pxi/pxi_dev.h"

namespace Service {
namespace PXI {

void InstallInterfaces(SM::ServiceManager& service_manager) {
    std::make_shared<PXI_DEV>()->InstallAsService(service_manager);
}

} // namespace PXI
} // namespace Service
