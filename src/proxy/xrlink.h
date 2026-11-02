// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
#pragma once
#include <cstdint>
#include "../common/ipc.h"

namespace avr {

void XrLinkInit();                                    // create shared memory, launch the 64-bit host
void XrLinkShutdown();
bool XrHostConnected();
bool XrWaitRecommendedSize(unsigned* w, unsigned* h, unsigned timeoutMs);
bool XrGetLatestPoses(IpcPose pose[2], IpcFov fov[2], int64_t* time);
void XrPublishTextures(unsigned w, unsigned h, unsigned format, const uint64_t handles[kSlots]);
void XrSubmitSlot(int slot, const IpcPose pose[2], const IpcFov fov[2], int64_t time, uint32_t flags);
int XrLastSubmittedSlot();
void XrRequestRecenter();
bool XrWaitHostFrame(unsigned ms);
struct VrSettings;
void XrPushSettings(const VrSettings& s);           // HUD / panel placement, gaze test mode
// In-headset settings menu: the shared panel texture (created by the game, opened by the host).
void XrPublishPanel(unsigned w, unsigned h, uint64_t handle);
void XrPanelUpdated(bool visible);
bool XrGaze(float* tx, float* ty, unsigned maxAgeMs);   // latest eye-gaze tangents, if fresh
bool XrGazeSupported();   // pace the game to the headset frame rate

} // namespace avr
