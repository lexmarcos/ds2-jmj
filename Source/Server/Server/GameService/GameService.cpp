/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Server/GameService/GameService.h"
#include "Server/GameService/GameClient.h"
#include "Server/GameService/GameManager.h"

#include "Server/Server.h"
#include "Server/Streams/Frpg2ReliableUdpPacketStream.h"
#include "Server/Streams/Frpg2ReliableUdpMessageStream.h"

#include "Shared/Core/Network/NetConnection.h"
#include "Shared/Core/Network/NetConnectionUDP.h"
#include "Shared/Core/Utils/Logging.h"
#include "Shared/Core/Utils/Strings.h"
#include "Shared/Core/Utils/DebugObjects.h"

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "Config/BuildConfig.h"
#include "Config/RuntimeConfig.h"

GameService::GameService(Server* OwningServer, RSAKeyPair* InServerRSAKey)
    : ServerInstance(OwningServer)
    , ServerRSAKey(InServerRSAKey)
{
}

GameService::~GameService()
{
}

void GameService::RegisterManager(std::shared_ptr<GameManager> Manager)
{
    Managers.push_back(Manager);
}

bool GameService::Init()
{
    ServerInstance->GetGameInterface().RegisterGameManagers(*this);

    Connection = std::make_shared<NetConnectionUDP>("Game Service");
    int Port = ServerInstance->GetConfig().GameServerPort;
    if (!Connection->Listen(Port))
    {
        Error("Game service failed to listen on port %i.", Port);
        return false;
    }

    Log("Game service is now listening on port %i.", Port);

    for (auto& Manager : Managers)
    {
        if (!Manager->Init())
        {
            Error("Failed to initialize game manager '%s'", Manager->GetName().c_str());
            return false;
        }
    }

    TrimDatabase();

    LoadAuthTokens();

    return true;
}

bool GameService::Term()
{
    SaveAuthTokens();

    for (auto& Manager : Managers)
    {
        if (!Manager->Term())
        {
            Error("Failed to terminate game manager '%s'", Manager->GetName().c_str());
            return false;
        }
    }

    return true;
}

namespace
{
    // One line per token: the token, the session key, and the wall clock time
    // it was last refreshed. Wall clock, because the point is to survive a
    // process that is about to lose its own clock.
    std::filesystem::path AuthTokenPath(Server* ServerInstance)
    {
        return ServerInstance->GetSavedPath() / "auth_tokens.txt";
    }

    std::string ToHex(const std::vector<uint8_t>& Bytes)
    {
        std::string Result;
        Result.reserve(Bytes.size() * 2);
        for (uint8_t Byte : Bytes)
        {
            char Pair[3];
            snprintf(Pair, sizeof(Pair), "%02x", Byte);
            Result += Pair;
        }
        return Result;
    }

    std::vector<uint8_t> FromHex(const std::string& Text)
    {
        std::vector<uint8_t> Result;
        if (Text.size() % 2 != 0)
        {
            return Result;
        }
        Result.reserve(Text.size() / 2);
        for (size_t Index = 0; Index + 1 < Text.size(); Index += 2)
        {
            Result.push_back(static_cast<uint8_t>(std::stoul(Text.substr(Index, 2), nullptr, 16)));
        }
        return Result;
    }
};

void GameService::SaveAuthTokens()
{
    if (!ServerInstance->GetConfig().PersistAuthTokens)
    {
        return;
    }

    std::filesystem::path Path = AuthTokenPath(ServerInstance);
    std::ofstream Output(Path, std::ios::trunc);
    if (!Output.is_open())
    {
        WarningS(GetName().c_str(), "Could not write authentication tokens to %s.", Path.string().c_str());
        return;
    }

    double Now = GetSeconds();
    int64_t WallNow = static_cast<int64_t>(time(nullptr));
    for (auto& Pair : AuthenticationStates)
    {
        // Store when each token was last used, not when it was written, so a
        // long session is not mistaken for a stale entry on the way back.
        int64_t LastUsed = WallNow - static_cast<int64_t>(Now - Pair.second.LastRefreshTime);
        Output << std::hex << Pair.second.AuthToken << " " << ToHex(Pair.second.CwcKey) << " "
               << std::dec << LastUsed << "\n";
    }
}

void GameService::LoadAuthTokens()
{
    if (!ServerInstance->GetConfig().PersistAuthTokens)
    {
        return;
    }

    std::filesystem::path Path = AuthTokenPath(ServerInstance);
    std::ifstream Input(Path);
    if (!Input.is_open())
    {
        return;
    }

    // A token nobody has used for a while belongs to a session that is over.
    // The window is generous compared with the timeout that applies while the
    // server runs, because the gap here is a restart, not idleness.
    const int64_t MaximumAge = 300;
    int64_t WallNow = static_cast<int64_t>(time(nullptr));

    std::string TokenText;
    std::string KeyText;
    int64_t LastUsed = 0;
    int Restored = 0;
    while (Input >> TokenText >> KeyText >> LastUsed)
    {
        if (WallNow - LastUsed > MaximumAge)
        {
            continue;
        }

        GameClientAuthenticationState AuthState;
        AuthState.AuthToken = std::strtoull(TokenText.c_str(), nullptr, 16);
        AuthState.CwcKey = FromHex(KeyText);
        // The clock this counts against started when this process did, so every
        // restored token gets a full window rather than a fabricated history.
        AuthState.LastRefreshTime = GetSeconds();
        if (AuthState.AuthToken == 0 || AuthState.CwcKey.empty())
        {
            continue;
        }

        AuthenticationStates.insert({ AuthState.AuthToken, AuthState });
        Restored++;
    }

    if (Restored > 0)
    {
        Log("Restored %i authentication token(s); clients from before the restart can carry on.", Restored);
    }
}

void GameService::TrimDatabase()
{
    Log("Trimming database entries.");

    for (auto& Manager : Managers)
    {
        Manager->TrimDatabase();
    }

    GetServer()->GetDatabase().Trim();

    NextDatabaseTrim = GetSeconds() + GetServer()->GetConfig().DatabaseTrimInterval;
}

void GameService::Poll()
{
    DebugTimerScope Scope(Debug::GameService_PollTime);

    Connection->Pump();

    for (auto& Manager : Managers)
    {
        Manager->Poll();
    }

    while (std::shared_ptr<NetConnection> ClientConnection = Connection->Accept())
    {
        HandleClientConnection(ClientConnection);
    }

    if (GetSeconds() > NextDatabaseTrim)
    {
        TrimDatabase();
    }

    for (auto iter = Clients.begin(); iter != Clients.end(); /* empty */)
    {
        std::shared_ptr<GameClient> Client = *iter;

        if (Client->Poll())
        {
            LogS(Client->GetName().c_str(), "Disconnecting client connection.");
            DisconnectingClients.push_back(Client);

            Client->MessageStream->Disconnect();

            // Let all managers know this client is being disconnected, they
            // may need to clean things up.
            for (auto& Manager : Managers)
            {
                Manager->OnLostPlayer(Client.get());
            }

            iter = Clients.erase(iter);
        }
        else
        {
            iter++;
        }
    }
    
    for (auto iter = DisconnectingClients.begin(); iter != DisconnectingClients.end(); /* empty */)
    {
        std::shared_ptr<GameClient> Client = *iter;

        Client->Connection->Pump();
        Client->MessageStream->Pump();

        if (Client->MessageStream->GetState() == Frpg2ReliableUdpStreamState::Closed)
        {
            LogS(Client->GetName().c_str(), "Client disconnected.");

            iter = DisconnectingClients.erase(iter);
        }
        else
        {
            iter++;
        }
    }

    // Remove authentication states that have timed out.
    for (auto iter = AuthenticationStates.begin(); iter != AuthenticationStates.end(); /* empty */)
    {
        auto& Pair = *iter;

        double ElapsedTime = GetSeconds() - Pair.second.LastRefreshTime;
        if (ElapsedTime > BuildConfig::AUTH_TICKET_TIMEOUT)
        {
            Verbose("Authentication token 0x%016llx has expired.", Pair.second.AuthToken);
            iter = AuthenticationStates.erase(iter);
        }
        else
        {
            iter++;
        }
    }

    // Written every few seconds rather than on every change: the file is tiny,
    // and this way a server that is killed rather than stopped still leaves a
    // recent copy behind.
    if (GetSeconds() - LastAuthTokenSaveTime > 5.0)
    {
        LastAuthTokenSaveTime = GetSeconds();
        SaveAuthTokens();
    }
}

void GameService::HandleClientConnection(std::shared_ptr<NetConnection> ClientConnection)
{
    uint64_t AuthToken;
    int BytesReceived = 0;

    std::vector<uint8_t> Buffer;
    Buffer.resize(sizeof(uint64_t));
    if (!ClientConnection->Peek(Buffer, 0, sizeof(AuthToken), BytesReceived) || BytesReceived != sizeof(AuthToken))
    {
        LogS(ClientConnection->GetName().c_str(), "Failed to peek authentication token, or not enough data available. Ignoring connection.");
        return;
    }

    AuthToken = *reinterpret_cast<uint64_t*>(Buffer.data());

    LogS(ClientConnection->GetName().c_str(), "Client connected.");

    // Check we have an authentication state for this client.
    auto AuthStateIter = AuthenticationStates.find(AuthToken);
    if (AuthStateIter == AuthenticationStates.end())
    {
        LogS(ClientConnection->GetName().c_str(), "Clients authentication token (0x%016llx) does not appear to be valid. Ignoring connection.", AuthToken);
        return;
    }

    GameClientAuthenticationState& AuthState = (*AuthStateIter).second;

    Debug::GameConnections.Add(1);

    std::shared_ptr<GameClient> Client = std::make_shared<GameClient>(this, ClientConnection, AuthState.CwcKey, AuthState.AuthToken);
    Clients.push_back(Client);

    // Let all managers know this client connected.
    for (auto& Manager : Managers)
    {
        Manager->OnGainPlayer(Client.get());
    }
}

std::string GameService::GetName()
{
    return "Game";
}

void GameService::CreateAuthToken(uint64_t AuthToken, const std::vector<uint8_t>& CwcKey)
{
    VerboseS(Connection->GetName().c_str(), "Created authentication token 0x%016llx", AuthToken);

    GameClientAuthenticationState AuthState;
    AuthState.AuthToken = AuthToken;
    AuthState.CwcKey = CwcKey;
    AuthState.LastRefreshTime = GetSeconds();
    AuthenticationStates.insert({ AuthToken, AuthState });
}

void GameService::RefreshAuthToken(uint64_t AuthToken)
{
    auto AuthStateIter = AuthenticationStates.find(AuthToken);
    if (AuthStateIter == AuthenticationStates.end())
    {
        return;
    }

    AuthStateIter->second.LastRefreshTime = GetSeconds();
}

std::shared_ptr<GameClient> GameService::FindClientByPlayerId(uint32_t PlayerId)
{
    for (std::shared_ptr<GameClient>& Client : Clients)
    {
        if (Client->GetPlayerState().GetPlayerId() == PlayerId)
        {
            return Client;
        }
    }
    return nullptr;
}

std::shared_ptr<GameClient> GameService::FindClientBySteamId(const std::string& SteamId)
{
    for (std::shared_ptr<GameClient>& Client : Clients)
    {
        if (Client->GetPlayerState().GetSteamId() == SteamId)
        {
            return Client;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<GameClient>> GameService::FindClients(std::function<bool(const std::shared_ptr<GameClient>&)> Predicate)
{
    std::vector<std::shared_ptr<GameClient>> Result;

    for (std::shared_ptr<GameClient>& Client : Clients)
    {
        if (Predicate(Client))
        {
            Result.push_back(Client);
        }
    }

    return Result;
}
