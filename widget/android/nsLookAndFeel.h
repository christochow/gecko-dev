/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef __nsLookAndFeel
#define __nsLookAndFeel

#include "nsXPLookAndFeel.h"
#include "AndroidBridge.h"

class nsLookAndFeel final : public nsXPLookAndFeel {
 public:
  nsLookAndFeel();
  virtual ~nsLookAndFeel();

  void NativeInit() final;
  virtual void RefreshImpl() override;
  virtual nsresult NativeGetColor(ColorID aID, nscolor& aResult) override;
  virtual nsresult GetIntImpl(IntID aID, int32_t& aResult) override;
  virtual nsresult GetFloatImpl(FloatID aID, float& aResult) override;
  virtual bool GetFontImpl(FontID aID, nsString& aName,
                           gfxFontStyle& aStyle) override;
  virtual bool GetEchoPasswordImpl() override;
  virtual uint32_t GetPasswordMaskDelayImpl() override;
  virtual char16_t GetPasswordCharacterImpl() override;
  LookAndFeelCache GetCacheImpl() override;
  void SetCacheImpl(const LookAndFeelCache& aCache) override;

 protected:
  bool mInitializedSystemColors = false;
  mozilla::AndroidSystemColors mSystemColors;
  bool mInitializedShowPassword = false;
  bool mShowPassword = false;

  bool mSystemUsesDarkTheme = false;
  bool mSystemUsesDarkThemeCached = false;

  bool mPrefersReducedMotion = false;
  bool mPrefersReducedMotionCached = false;

  nsresult GetSystemColors();

  void EnsureInitSystemColors();
  void EnsureInitShowPassword();
};

#endif
