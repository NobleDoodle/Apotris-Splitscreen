#include "matchmakingQueue.hpp"

#if !defined(GBA) && !defined(MULTIBOOT)

#include "def.h"
#include "localLink.hpp"
#include "multiplayerClasses.h"
#include "platform.hpp"
#include "scene.hpp"
#include "sceneModes.hpp"

namespace MatchmakingQueue {
namespace {

bool queued = false;
bool suspended = false;
bool matchFoundScenePending = false;
bool returnToGame = false;
bool transitioningToBattle = false;
bool transitioningFromMatchFound = false;
bool returnToQueueOnConnectionFailure = false;
int spinnerAngle = 0;

} // namespace

void start() {
    multiplayerStart = false;
    queued = true;
    suspended = false;
    matchFoundScenePending = false;
    returnToGame = false;
    transitioningToBattle = false;
    transitioningFromMatchFound = false;
    returnToQueueOnConnectionFailure = false;
    multiplayerLink->activate();
}

void startLocal(bool fade) {
    roomId.clear();
    start();
    localLinkInvite();
    if (fade)
        changeScene([]() { return new QueueWaitingScene(); },
                    Transitions::FADE);
    else
        changeScene([]() { return new QueueWaitingScene(); });
}

void cancel() {
    if (multiplayerLink->active)
        multiplayerLink->deactivate(true);

    queued = false;
    suspended = false;
    matchFoundScenePending = false;
    transitioningToBattle = false;
    returnToQueueOnConnectionFailure = false;
}

void acceptMatch() {
    returnToQueueOnConnectionFailure = true;
    queued = false;
    suspended = false;
    matchFoundScenePending = false;
    transitioningToBattle = true;
    changeScene([]() { return new MultBattleScene(); }, Transitions::FADE);
}

void denyMatch() {
    const bool resumeExistingGame = returnToGame;
    cancel();
    transitioningFromMatchFound = true;
    if (resumeExistingGame)
        changeScene([]() { return new GameScene(true); });
    else
        changeScene([]() { return new ModeListScene(); }, Transitions::FADE);
}

void activateMatchFoundScene() { matchFoundScenePending = false; }

void matchStarted() { returnToQueueOnConnectionFailure = false; }

bool isQueued() { return queued; }

bool returnToWaitingAfterConnectionFailure() {
    if (!returnToQueueOnConnectionFailure)
        return false;

    cancel();
    start();
    changeScene([]() { return new QueueWaitingScene(); }, Transitions::FADE);
    return true;
}

void update() {
    // Split screen: the other player opened a battle, so queue for it too.
    // Being queued is what brings up MATCH FOUND (Accept/Deny) here.
    localLinkPoll();
    if (!queued && !multiplayerLink->active && localLinkTakeInvite())
        start();

    if (transitioningFromMatchFound) {
        if (scene->isMatchFoundScene())
            return;
        transitioningFromMatchFound = false;
    }

    if (transitioningToBattle) {
        if (dynamic_cast<MultBattleScene*>(scene))
            transitioningToBattle = false;
        else
            return;
    }

    if (!queued)
        return;

    if (scene->suspendsMatchmaking()) {
        if (!suspended) {
            suspended = true;
            if (multiplayerLink->active)
                multiplayerLink->deactivate(false);
        }
        return;
    }

    if (suspended) {
        suspended = false;
        multiplayerLink->activate();
    }

    const bool syncOk = multiplayerLink->sync();
    const bool matchReady =
        multiplayerLink->stateMachine
            .IsInState<MultiplayerStates::PlayersAwaitReady>() &&
        multiplayerLink->universal->playerCount() > 1;

    if ((matchFoundScenePending || scene->isMatchFoundScene()) &&
        (!syncOk || !multiplayerLink->universal->isConnected() ||
         !matchReady)) {
        if (scene->isMatchFoundScene())
            denyMatch();
        else
            cancel();
        return;
    }

    if (matchReady) {
        if (scene->isQueueWaitingScene()) {
            acceptMatch();
        } else if (!matchFoundScenePending && !scene->isMatchFoundScene()) {
            // GameScene may be recreated on denial because the live Game is
            // global. It resets presentation and runs the normal countdown.
            returnToGame = dynamic_cast<GameScene*>(scene) != nullptr;
            matchFoundScenePending = true;
            changeScene([resumeExistingGame = returnToGame]() {
                return new MatchFoundScene(resumeExistingGame);
            });
        }
    }
}

bool blocksSceneUpdate() {
    return matchFoundScenePending || transitioningToBattle ||
           transitioningFromMatchFound;
}

bool isTransitioningToBattle() { return transitioningToBattle; }

void drawOverlay() {
    if (queued && !suspended && !matchFoundScenePending &&
        !scene->isMatchFoundScene()) {
        constexpr int spinnerSprite = 109;
        constexpr int spinnerAffine = 27;
        constexpr int spinnerTile = 1000;

        loadSpriteTiles(spinnerTile, blockSprite, 1, 1);
        OBJ_ATTR* spinner = &obj_buffer[spinnerSprite];
        sprite_set_attr(spinner, ShapeSquare, 0, spinnerTile, 5, 0);
        sprite_enable_affine(spinner, spinnerAffine, true);
        sprite_rotscale(spinner, int2fx(1), int2fx(1), spinnerAngle,
                        spinnerAffine);
        const int width = (savefile->settings.aspectRatio == 0) ? 240 : 214;
        const int left = (240 - width) / 2;
        sprite_set_pos(spinner, left + width - 20, 4);
        sprite_unhide(spinner, ATTR0_AFF_DBL);
        spinnerAngle = (spinnerAngle + 1024) & 0xffff;
    } else {
        sprite_hide(&obj_buffer[109]);
    }

    showSprites(128);
}

} // namespace MatchmakingQueue

#else

namespace MatchmakingQueue {
void start() {}
void startLocal(bool) {}
void cancel() {}
void acceptMatch() {}
void denyMatch() {}
void activateMatchFoundScene() {}
void matchStarted() {}
bool isQueued() { return false; }
bool returnToWaitingAfterConnectionFailure() { return false; }
void update() {}
void drawOverlay() {}
bool blocksSceneUpdate() { return false; }
bool isTransitioningToBattle() { return false; }
} // namespace MatchmakingQueue

#endif
