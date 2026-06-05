#pragma once

#include <string>

enum class PacketType
{
    JoinGame,
    SubmitBluff,
    SelectVote,
    ChangeName,
    Disconnected,

    PlayerJoined,
    PlayerChangedName,
    PlayerLeave,
    GameStarted,
    SendQuestion,
    TimerTick,
    SendVoteOptions, 
    RoundResults,
    GameOver,

    GetHelper,
    Wrong,
};

struct GamePacket
{
    PacketType Type;
    std::string Data;
};