/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "GamepadEventChannelParent.h"
#include "GamepadPlatformService.h"
#include "mozilla/dom/GamepadMonitoring.h"
#include "nsThreadUtils.h"

namespace mozilla {
namespace dom {

using namespace mozilla::ipc;

namespace {

class SendGamepadUpdateRunnable final : public Runnable {
 private:
  ~SendGamepadUpdateRunnable() = default;
  RefPtr<GamepadEventChannelParent> mParent;
  GamepadChangeEvent mEvent;

 public:
  SendGamepadUpdateRunnable(GamepadEventChannelParent* aParent,
                            GamepadChangeEvent aEvent)
      : Runnable("dom::SendGamepadUpdateRunnable"),
        mParent(aParent),
        mEvent(aEvent) {
    MOZ_ASSERT(mParent);
  }
  NS_IMETHOD Run() override {
    AssertIsOnBackgroundThread();
    Unused << mParent->SendGamepadUpdate(mEvent);
    return NS_OK;
  }
};

}  // namespace

bool GamepadEventChannelParent::Init() {
  AssertIsOnBackgroundThread();

  mBackgroundEventTarget = GetCurrentEventTarget();

  RefPtr<GamepadPlatformService> service =
      GamepadPlatformService::GetParentService();
  MOZ_ASSERT(service);

  service->AddChannelParent(this);

  return true;
}

void GamepadEventChannelParent::ActorDestroy(ActorDestroyReason aWhy) {
  AssertIsOnBackgroundThread();

  RefPtr<GamepadPlatformService> service =
      GamepadPlatformService::GetParentService();
  MOZ_ASSERT(service);
  service->RemoveChannelParent(this);
}

mozilla::ipc::IPCResult GamepadEventChannelParent::RecvVibrateHaptic(
    const uint32_t& aControllerIdx, const uint32_t& aHapticIndex,
    const double& aIntensity, const double& aDuration,
    const uint32_t& aPromiseID) {
  // TODO: Bug 680289, implement for standard gamepads

  if (SendReplyGamepadPromise(aPromiseID)) {
    return IPC_OK();
  }

  return IPC_FAIL(this, "SendReplyGamepadPromise fail.");
}

mozilla::ipc::IPCResult GamepadEventChannelParent::RecvStopVibrateHaptic(
    const uint32_t& aControllerIdx) {
  // TODO: Bug 680289, implement for standard gamepads
  return IPC_OK();
}

mozilla::ipc::IPCResult GamepadEventChannelParent::RecvLightIndicatorColor(
    const uint32_t& aControllerIdx, const uint32_t& aLightColorIndex,
    const uint8_t& aRed, const uint8_t& aGreen, const uint8_t& aBlue,
    const uint32_t& aPromiseID) {
  SetGamepadLightIndicatorColor(aControllerIdx, aLightColorIndex, aRed, aGreen,
                                aBlue);

  if (SendReplyGamepadPromise(aPromiseID)) {
    return IPC_OK();
  }

  return IPC_FAIL(this, "SendReplyGamepadPromise fail.");
}

void GamepadEventChannelParent::DispatchUpdateEvent(
    const GamepadChangeEvent& aEvent) {
  mBackgroundEventTarget->Dispatch(new SendGamepadUpdateRunnable(this, aEvent),
                                   NS_DISPATCH_NORMAL);
}

}  // namespace dom
}  // namespace mozilla
