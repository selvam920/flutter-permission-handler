#include "include/permission_handler_windows/permission_handler_windows_plugin.h"

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <flutter/event_channel.h>
#include <flutter/event_stream_handler.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/encodable_value.h>
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <map>
#include <string>
#include <variant>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Geolocation.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Collections.h>

#include "permission_constants.h"

namespace {

using namespace flutter;
using namespace winrt;
using namespace winrt::Windows::Devices::Geolocation;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Radios;

template<typename T>
T GetArgument(const std::string arg, const EncodableValue* args, T fallback) {
  T result {fallback};
  const auto* arguments = std::get_if<EncodableMap>(args);
  if (arguments) {
    auto result_it = arguments->find(EncodableValue(arg));
    if (result_it != arguments->end()) {
      result = std::get<T>(result_it->second);
    }
  }
  return result;
}

class PermissionHandlerWindowsPlugin : public Plugin {
 public:
  static void RegisterWithRegistrar(PluginRegistrar* registrar);

  PermissionHandlerWindowsPlugin();

  virtual ~PermissionHandlerWindowsPlugin();

  // Disallow copy and move.
  PermissionHandlerWindowsPlugin(const PermissionHandlerWindowsPlugin&) = delete;
  PermissionHandlerWindowsPlugin& operator=(const PermissionHandlerWindowsPlugin&) = delete;

  // Called when a method is called on the plugin channel.
  void HandleMethodCall(const MethodCall<>&,
                        std::unique_ptr<MethodResult<>>);

 private:
  void IsLocationServiceEnabled(std::unique_ptr<MethodResult<>> result);
  void IsBluetoothServiceEnabled(std::unique_ptr<MethodResult<>> result);
  winrt::Windows::Foundation::IAsyncOperation<int32_t> GetBluetoothServiceStatusAsync();

  winrt::Windows::Devices::Geolocation::Geolocator geolocator;
  winrt::Windows::Devices::Geolocation::Geolocator::PositionChanged_revoker m_positionChangedRevoker;
};

// static
void PermissionHandlerWindowsPlugin::RegisterWithRegistrar(
    PluginRegistrar* registrar) {

  auto channel = std::make_unique<MethodChannel<>>(
    registrar->messenger(), "flutter.baseflow.com/permissions/methods",
    &StandardMethodCodec::GetInstance());

  std::unique_ptr<PermissionHandlerWindowsPlugin> plugin = std::make_unique<PermissionHandlerWindowsPlugin>();

  channel->SetMethodCallHandler(
    [plugin_pointer = plugin.get()](const auto& call, auto result) {
      plugin_pointer->HandleMethodCall(call, std::move(result));
    });

  registrar->AddPlugin(std::move(plugin));
}

PermissionHandlerWindowsPlugin::PermissionHandlerWindowsPlugin(){
  m_positionChangedRevoker = geolocator.PositionChanged(winrt::auto_revoke,
    [this](Geolocator const& geolocator, PositionChangedEventArgs e)
    {
    });
}

PermissionHandlerWindowsPlugin::~PermissionHandlerWindowsPlugin() = default;

void PermissionHandlerWindowsPlugin::HandleMethodCall(
    const MethodCall<>& method_call,
    std::unique_ptr<MethodResult<>> result) {
  
  auto methodName = method_call.method_name();
  if (methodName.compare("checkServiceStatus") == 0) {
    auto permission = (PermissionConstants::PermissionGroup)std::get<int>(*method_call.arguments());
    if (permission == PermissionConstants::PermissionGroup::LOCATION ||
        permission == PermissionConstants::PermissionGroup::LOCATION_ALWAYS ||
        permission == PermissionConstants::PermissionGroup::LOCATION_WHEN_IN_USE) {
        IsLocationServiceEnabled(std::move(result));
        return;
    }
    if(permission == PermissionConstants::PermissionGroup::BLUETOOTH){
        IsBluetoothServiceEnabled(std::move(result));
        return;
    }

    if (permission == PermissionConstants::PermissionGroup::IGNORE_BATTERY_OPTIMIZATIONS) {
        result->Success(EncodableValue((int)PermissionConstants::ServiceStatus::ENABLED));
        return;
    }

    result->Success(EncodableValue((int)PermissionConstants::ServiceStatus::NOT_APPLICABLE));
    
  } else if (methodName.compare("checkPermissionStatus") == 0) {
    result->Success(EncodableValue((int)PermissionConstants::PermissionStatus::GRANTED));
  } else if (methodName.compare("requestPermissions") == 0) {
    auto permissionsEncoded = std::get<EncodableList>(*method_call.arguments());
    std::vector<int> permissions;
    permissions.reserve( permissionsEncoded.size() );
    std::transform( permissionsEncoded.begin(), permissionsEncoded.end(),
                    std::back_inserter( permissions ),
                    [](const EncodableValue& encoded) {
                      return std::get<int>(encoded);
                    });
    
    EncodableMap requestResults;

    for (int i=0;i<permissions.size();i++) {
      auto permissionStatus = PermissionConstants::PermissionStatus::GRANTED;
      requestResults.insert({EncodableValue(permissions[i]), EncodableValue((int)permissionStatus)});
    }

    result->Success(requestResults);
  } else if (methodName.compare("shouldShowRequestPermissionRationale") == 0
          || methodName.compare("openAppSettings")) {
    result->Success(EncodableValue(false));
  } else {
    result->NotImplemented();
  }
}

void PermissionHandlerWindowsPlugin::IsLocationServiceEnabled(std::unique_ptr<MethodResult<>> result) {
  result->Success(EncodableValue((int)(geolocator.LocationStatus() != PositionStatus::NotAvailable
        ? PermissionConstants::ServiceStatus::ENABLED
        : PermissionConstants::ServiceStatus::DISABLED)));
}

void PermissionHandlerWindowsPlugin::IsBluetoothServiceEnabled(std::unique_ptr<MethodResult<>> result) {
  auto method_result = std::shared_ptr<MethodResult<>>(result.release());
  auto status_operation = GetBluetoothServiceStatusAsync();

  status_operation.Completed(
      [method_result](const auto& async_operation,
                      winrt::Windows::Foundation::AsyncStatus async_status) {
        int32_t service_status = static_cast<int32_t>(PermissionConstants::ServiceStatus::DISABLED);

        if (async_status == winrt::Windows::Foundation::AsyncStatus::Completed) {
          try {
            service_status = async_operation.GetResults();
          } catch (const winrt::hresult_error&) {
            service_status = static_cast<int32_t>(PermissionConstants::ServiceStatus::DISABLED);
          }
        }

        method_result->Success(EncodableValue(static_cast<int>(service_status)));
      });
}

winrt::Windows::Foundation::IAsyncOperation<int32_t>
PermissionHandlerWindowsPlugin::GetBluetoothServiceStatusAsync() {
  auto bt_adapter = co_await BluetoothAdapter::GetDefaultAsync();

  if (bt_adapter == nullptr || !bt_adapter.IsCentralRoleSupported()) {
    co_return static_cast<int32_t>(PermissionConstants::ServiceStatus::DISABLED);
  }

  auto radios = co_await Radio::GetRadiosAsync();

  for (uint32_t i = 0; i < radios.Size(); i++) {
    auto radio = radios.GetAt(i);
    if (radio.Kind() == RadioKind::Bluetooth) {
      co_return radio.State() == RadioState::On
          ? static_cast<int32_t>(PermissionConstants::ServiceStatus::ENABLED)
          : static_cast<int32_t>(PermissionConstants::ServiceStatus::DISABLED);
    }
  }

  co_return static_cast<int32_t>(PermissionConstants::ServiceStatus::DISABLED);
}

}  // namespace

void PermissionHandlerWindowsPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  PermissionHandlerWindowsPlugin::RegisterWithRegistrar(
      PluginRegistrarManager::GetInstance()
          ->GetRegistrar<PluginRegistrarWindows>(registrar));
}
