//////////////////////////////////////////////////////////////////////
// OpenTibia - an opensource roleplaying game
//////////////////////////////////////////////////////////////////////
//
//////////////////////////////////////////////////////////////////////
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//////////////////////////////////////////////////////////////////////
#include "otpch.h"

#include "connection.h"
#include "outputmessage.h"
#include "protocollogin.h"

#include "ban.h"
#include "configmanager.h"
#include "game.h"
#include "ioaccount.h"
#include "tools.h"
#ifdef __PROTOCOL_77__
#include "rsa.h"
#endif // __PROTOCOL_77__

#include <iomanip>

extern ConfigManager g_config;
extern IPList serverIPs;
extern BanManager g_bans;
extern Game g_game;
#ifdef __PROTOCOL_77__
extern RSA* g_otservRSA;
#endif // __PROTOCOL_77__

#ifdef __ENABLE_SERVER_DIAGNOSTIC__
uint32_t ProtocolLogin::protocolLoginCount = 0;
#endif

#ifdef __DEBUG_NET_DETAIL__
void ProtocolLogin::deleteProtocolTask()
{
	std::cout << "Deleting ProtocolLogin" << std::endl;
	Protocol::deleteProtocolTask();
}
#endif

void ProtocolLogin::disconnectClient(uint8_t error, const char* message)
{
	OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false);
	if (output) {
		TRACK_MESSAGE(output);
		output->AddByte(error);
		output->AddString(message);
		OutputMessagePool::getInstance()->send(output);
	}

	getConnection()->closeConnection();
}


bool ProtocolLogin::parseFirstPacket(NetworkMessage& msg)
{
	if (g_game.getGameState() == GAME_STATE_SHUTDOWN) {
		getConnection()->closeConnection();
		return false;
	}

	uint32_t clientip = getConnection()->getIP();

	/*uint16_t clientos =*/msg.GetU16();
	uint16_t version = msg.GetU16();
	msg.SkipBytes(12);

	if (version < CLIENT_VERSION_MIN || version > CLIENT_VERSION_MAX) {
		disconnectClient(0x0A, STRING_CLIENT_VERSION);
		return false;
	}

#ifdef __PROTOCOL_77__
	if (!RSA_decrypt(g_otservRSA, msg)) {
		getConnection()->closeConnection();
		return false;
	}

	uint32_t key[4];
	key[0] = msg.GetU32();
	key[1] = msg.GetU32();
	key[2] = msg.GetU32();
	key[3] = msg.GetU32();
	enableXTEAEncryption();
	setXTEAKey(key);
#endif // __PROTOCOL_77__

	uint32_t accnumber = msg.GetU32();
	std::string password = msg.GetString();

	if (!accnumber) {
		disconnectClient(0x0A, "You must enter your account number.");
		return false;
	}

	if (g_game.getGameState() == GAME_STATE_STARTUP) {
		disconnectClient(0x0A, "Gameworld is starting up. Please wait.");
		return false;
	}

	if (g_bans.isDeleted(accnumber)) {
		disconnectClient(0x0A, "Your account has been deleted!");
		return false;
	}

	if (g_bans.isIpDisabled(clientip)) {
		disconnectClient(0x0A,
		                 "Too many connections attempts from this IP. Try again later.");
		return false;
	}

	if (g_bans.isIpBanished(clientip)) {
		disconnectClient(0x0A, "Your IP is banished!");
		return false;
	}

	uint32_t serverip = serverIPs[0].first;
	for (uint32_t i = 0; i < serverIPs.size(); i++) {
		if ((serverIPs[i].first & serverIPs[i].second) == (clientip & serverIPs[i].second)) {
			serverip = serverIPs[i].first;
			break;
		}
	}

	Account account = IOAccount::instance()->loadAccount(accnumber);
	if (!(accnumber != 0 && account.accnumber == accnumber && passwordTest(password, account.password))) {

		g_bans.addLoginAttempt(clientip, false);
		disconnectClient(0x0A, "Please enter a valid account number and password.");
		return false;
	}

	g_bans.addLoginAttempt(clientip, true);

	OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false);
	if (output) {
		TRACK_MESSAGE(output);
		// Add MOTD
		std::stringstream motd;
		output->AddByte(0x14);
		motd << g_config.getNumber(ConfigManager::MOTD_NUM) << "\n";
		motd << g_config.getString(ConfigManager::MOTD);
		output->AddString(motd.str());
		// Add char list
		output->AddByte(0x64);
		output->AddByte((uint8_t)account.charList.size());

		for (auto& name : account.charList) {
			output->AddString(name);
			output->AddString(g_config.getString(ConfigManager::WORLD_NAME));
			output->AddU32(serverip);
			output->AddU16(g_config.getNumber(ConfigManager::PORT));
		}

		// Add premium days
		output->AddU16(Account::getPremiumDaysLeft(account.premEnd));

		OutputMessagePool::getInstance()->send(output);
	}

	getConnection()->closeConnection();

	return true;
}

void ProtocolLogin::onRecvFirstMessage(NetworkMessage& msg)
{
	parseFirstPacket(msg);
}
