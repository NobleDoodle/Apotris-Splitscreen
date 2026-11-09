#pragma once

namespace MatchmakingQueue {

void start();
// Split screen: queue for a battle with the other half of the screen and
// wait in the queue screen, which joins as soon as both sides are in.
void startLocal(bool fade = true);
void cancel();
void acceptMatch();
void denyMatch();
void activateMatchFoundScene();
void matchStarted();
bool isQueued();
bool returnToWaitingAfterConnectionFailure();
void update();
void drawOverlay();

bool blocksSceneUpdate();
bool isTransitioningToBattle();

} // namespace MatchmakingQueue
