// Stub network layer used when DETHRACE_NET_ENABLED is off (for example on the
// Dreamcast build). Implements the pd/net.h contract with no-ops so the game
// links and runs single player. Hosting or joining a network game does nothing.

#include "pd/net.h"

#include "dr_types.h"
#include "harness/trace.h"

void ClearupPDNetworkStuff(void) {
}

void MATTMessageCheck(char* pFunction_name, tNet_message* pMessage, int pAlleged_size) {
}

int GetMessageTypeFromMessage(char* pMessage_str) {
    return 0;
}

void MakeMessageToSend(int pMessage_type) {
}

int ReceiveHostResponses(void) {
    return 0;
}

int BroadcastMessage(void) {
    return 0;
}

int PDNetInitialise(void) {
    return -1;
}

int PDNetShutdown(void) {
    return 0;
}

void PDNetStartProducingJoinList(void) {
}

void PDNetEndJoinList(void) {
}

int PDNetGetNextJoinGame(tNet_game_details* pGame, int pIndex) {
    return 0;
}

void PDNetDisposeGameDetails(tNet_game_details* pDetails) {
}

int PDNetHostGame(tNet_game_details* pDetails, char* pHost_name, void** pHost_address) {
    return -1;
}

int PDNetJoinGame(tNet_game_details* pDetails, char* pPlayer_name) {
    return -1;
}

void PDNetLeaveGame(tNet_game_details* pDetails) {
}

void PDNetHostFinishGame(tNet_game_details* pDetails) {
}

tU32 PDNetExtractGameID(tNet_game_details* pDetails) {
    return 0;
}

tPlayer_ID PDNetExtractPlayerID(tNet_game_details* pDetails) {
    return 0;
}

void PDNetObtainSystemUserName(char* pName, int pMax_length) {
    if (pMax_length > 0) {
        pName[0] = '\0';
    }
}

int PDNetSendMessageToPlayer(tNet_game_details* pDetails, tNet_message* pMessage, tPlayer_ID pPlayer) {
    return -1;
}

int PDNetSendMessageToAllPlayers(tNet_game_details* pDetails, tNet_message* pMessage) {
    return -1;
}

tNet_message* PDNetGetNextMessage(tNet_game_details* pDetails, void** pSender_address) {
    return NULL;
}

tNet_message* PDNetAllocateMessage(tU32 pSize, tS32 pSize_decider) {
    return NULL;
}

void PDNetDisposeMessage(tNet_game_details* pDetails, tNet_message* pMessage) {
}

void PDNetSetPlayerSystemInfo(tNet_game_player_info* pPlayer, void* pSender_address) {
}

void PDNetDisposePlayer(tNet_game_player_info* pPlayer) {
}

int PDNetSendMessageToAddress(tNet_game_details* pDetails, tNet_message* pMessage, void* pAddress) {
    return -1;
}

int PDNetInitClient(tNet_game_details* pDetails) {
    return -1;
}

int PDNetGetHeaderSize(void) {
    return 0;
}
