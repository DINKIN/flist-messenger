
#include <QTime>

#include "flist_session.h"
#include "flist_account.h"
#include "flist_global.h"
#include "flist_server.h"
#include "flist_character.h"
#include "flist_iuserinterface.h"


#include "../libjson/libJSON.h"

FSession::FSession(FAccount *account, QString &character, QObject *parent) :
	QObject(parent),
	connected(false),
	account(account),
	character(character),
	tcpsocket(0),
	characterlist(),
	operatorlist(),
	wsready(false),
	socketreadbuffer()
{
}

FSession::~FSession()
{
	if(tcpsocket != 0) {
		delete tcpsocket;
		tcpsocket = 0;
	}
}

//todo: All the web socket stuff should really go into its own class.
void FSession::connectSession()
{
	debugMessage("session->connectSession()");
	if(connected) {
		return;
	}

	connected = true;
	wsready = false;
	
        tcpsocket = new QTcpSocket ( this );
        //tcpsocket = new QSslSocket ( this );
        //tcpsocket->ignoreSslErrors();
	debugMessage("Connecting...");
        //tcpsocket->connectToHost (FLIST_CHAT_SERVER, FLIST_CHAT_SERVER_PORT);
	tcpsocket->connectToHost (account->server->chatserver_host, account->server->chatserver_port);
        //tcpsocket->connectToHostEncrypted ( "chat.f-list.net", FLIST_PORT );
        connect ( tcpsocket, SIGNAL ( connected() ), this, SLOT ( socketConnected() ) );
        //connect ( tcpsocket, SIGNAL ( encrypted() ), this, SLOT ( socketSslConnected() ) );
        connect ( tcpsocket, SIGNAL ( readyRead() ), this, SLOT ( socketReadReady() ) );
        connect ( tcpsocket, SIGNAL ( error ( QAbstractSocket::SocketError ) ), this, SLOT ( socketError ( QAbstractSocket::SocketError ) ) );
        //connect ( tcpsocket, SIGNAL ( sslErrors( QList<QSslError> ) ), this, SLOT ( socketSslError ( QList<QSslError> ) ) );
}

void FSession::socketConnected()
{
	debugMessage("Connected.");
	//todo: this should use a better random source
	srand(QTime::currentTime().msecsTo(QTime()));
	unsigned char nonce[16];
	int i;
	for(i = 0; i < 16; i++) {
		nonce[i] = (unsigned char) rand();
	}

	QString header;
	header.sprintf( 
		       "GET / HTTP/1.1\r\n"
		       "Upgrade: WebSocket\r\n"
		       "Connection: Upgrade\r\n"
		       "Host: f-list.net:%d\r\n"
		       "Origin: https://%s\r\n"
		       "Sec-WebSocket-Key: %s\r\n"
		       "Sec-WebSocket-Version: 8\r\n"
		       "\r\n"
		       , account->server->chatserver_port, (const char *)account->server->chatserver_host.toUtf8()
		       , (const char *)QByteArray((const char *)nonce, 16).toBase64());

	tcpsocket->write ( header.toUtf8() );
	tcpsocket->flush();
	
}

void FSession::socketError(QAbstractSocket::SocketError error)
{
	connected = false;
	emit socketErrorSignal(error);
	if(tcpsocket) {
		tcpsocket->abort();
		tcpsocket->deleteLater();
		tcpsocket = 0;
	}
}

void FSession::socketReadReady()
{
	if(!wsready) {
		QByteArray buffer = tcpsocket->readAll();
		std::string buf(socketreadbuffer);
		buf.append( buffer.begin(), buffer.end() );
		if ( buf.find ( "\r\n\r\n" ) == std::string::npos ) {
			//debugMessage("WebSocket waiting...");
			//debugMessage(buf);
			//debugMessage("...");
			socketreadbuffer = buf;
			return;
		} else {
			debugMessage("WebSocket connected.");
			wsready = true;
			socketreadbuffer.clear();
		}
		//todo: verify "Sec-WebSocket-Accept" response
		JSONNode loginnode;
		loginnode.push_back(JSONNode("method", "ticket"));
		loginnode.push_back(JSONNode("ticket", account->ticket.toStdString()));
		loginnode.push_back(JSONNode("account", account->getUserName().toStdString()));
		loginnode.push_back(JSONNode("cname", FLIST_CLIENTID));
		loginnode.push_back(JSONNode("cversion", FLIST_VERSIONNUM));
		loginnode.push_back(JSONNode("character", character.toStdString()));
		std::string idenStr = "IDN " + loginnode.write();
		//debugMessage("Indentify...");
		wsSend(idenStr);
        } else {
		//debugMessage("receiving...");
		QByteArray buffer = tcpsocket->readAll();
		std::string buf(socketreadbuffer);
		buf.append(buffer.begin(), buffer.end());
		unsigned int lengthsize;
		unsigned int payloadlength;
		unsigned int headersize;
		unsigned int i;
		while(1) {
			if(buf.length() < 2) {
				break;
			}
			payloadlength = buf[1] & 0x7f;
			if(payloadlength < 126) {
				lengthsize = 0;
			} else if(payloadlength == 126) {
				lengthsize = 2;
				if(buf.length() < 4) {
					break;
				}
				payloadlength = ((buf[2] & 0xff) << 8) | (buf[3] & 0xff);
			} else {
				lengthsize = 8;
				if(buf.length() < 10) {
					break;
				}
				//Does not handle lengths greater than 4GB
				payloadlength = ((buf[6] & 0xff) << 24) | ((buf[7] & 0xff) << 16) | ((buf[8] & 0xff) << 8) | (buf[9] & 0xff);
			}
			if(buf[1] & 0x80) {
				headersize = lengthsize + 2 + 4;
			} else {
				headersize = lengthsize + 2;
			}
			//todo: sanity check the opcode, final fragment and reserved bits
			//if(buf != 0x81) {
			//        display error
			//        disconnect?
			//}
			if(buf.length() < headersize + payloadlength) {
				break;
			}
			std::string cmd = buf.substr ( headersize, payloadlength );
			if(buf[1] & 0x80) {
				for(i = 0; i < payloadlength; i++) {
					cmd[i] ^= buf[lengthsize + 2 + (i & 0x3)];
				}
			}
			wsRecv(cmd);
			if ( buf.length() <= headersize + payloadlength)
			{
				buf.clear();
				break;
			} else {
				buf = buf.substr ( headersize + payloadlength, buf.length() - (headersize + payloadlength) );
			}
		}
		socketreadbuffer = buf;
	}	
}

void FSession::wsSend(std::string &input)
{
	if(!connected) {
		//textEdit->append ( "Attempted to send a message, but client is disconnected." );
	} else {
		fix_broken_escaped_apos ( input );
		debugMessage( ">>" + input);
		QByteArray buf;
		QDataStream stream ( &buf, QIODevice::WriteOnly );
		input.resize ( input.length() );
		//Send WS frame as a single text frame
		stream << ( quint8 ) 0x81;
		//Length of frame with mask bit sent
		if(input.length() < 126) {
			stream << ( quint8 ) (input.length() | 0x80);
		} else if(input.length() < 0x10000) {
			stream << ( quint8 ) (126 | 0x80);
			stream << ( quint8 ) (input.length() >> 8);
			stream << ( quint8 ) (input.length() >> 0);
		} else {
			//Does not handle the case if we're trying to send more than 4GB.
			stream << ( quint8 ) (127 | 0x80);
			stream << ( quint8 ) (0x00);
			stream << ( quint8 ) (0x00);
			stream << ( quint8 ) (0x00);
			stream << ( quint8 ) (0x00);
			stream << ( quint8 ) (input.length() >> 24);
			stream << ( quint8 ) (input.length() >> 16);
			stream << ( quint8 ) (input.length() >> 8);
			stream << ( quint8 ) (input.length());
		}
		//The mask to use for this frame.
		//The spec says it should be cryptographically strong random number, but we're using a weak random source instead.
		quint8 mask[4];
        unsigned int i;

		for(i = 0; i < 4; i++) {
			mask[i] = rand() & 0xff;
			stream << mask[i];
		}
                
		for(i = 0; i < input.length(); i++) {
			stream << (quint8)(input[i] ^ mask[i & 0x3]);
		}
		tcpsocket->write(buf);
		tcpsocket->flush();
	}
	
}

void FSession::wsRecv(std::string packet)
{
	debugMessage("<<" + packet);
	try {
		std::string cmd = packet.substr(0, 3);
		JSONNode nodes;
		if(packet.length() > 4) {
			nodes = libJSON::parse(packet.substr(4, packet.length() - 4));
		}
#define CMD(name) if(cmd == #name) {cmd##name(packet, nodes); return;}
		CMD(ADL); //List of all chat operators.
		CMD(AOP); //Add a chat operator.
		CMD(DOP); //Remove a chat operator.
		CMD(PIN); //Ping.
		emit processCommand(packet, cmd, nodes);
	} catch(std::invalid_argument) {
                debugMessage("Server returned invalid json in its response: " + packet);
        } catch(std::out_of_range) {
                debugMessage("Server produced unexpected json without a field we expected: " + packet);
        }
}

#define COMMAND(name) void FSession::cmd##name(std::string &rawpacket, JSONNode &nodes)


//todo: Merge common code in ADL, AOP and DOP into a separate function.
COMMAND(ADL)
{
	(void)rawpacket;
	//The list of current chat-ops.
	//ADL {"ops": ["name1", "name2"]}
	JSONNode childnode = nodes.at("ops");
	int size = childnode.size();

	for(int i = 0; i < size; ++i) {
		QString op = childnode[i].as_string().c_str();
		operatorlist.append(op.toLower());

		if(isCharacterOnline(op)) {
			// Set flag in character
			FCharacter* character = characterlist[op];
			character->setIsChatOp(true);
		}
		account->ui->setChatOperator(this, op, true);
	}
	
}
COMMAND(AOP)
{
	(void)rawpacket;
	//Add a character to the list of known chat-operators.
	//AOP {"character": "Viona"}
	QString op = nodes.at("character").as_string().c_str();
	operatorlist.append(op.toLower());
	
	if(isCharacterOnline(op)) {
		// Set flag in character
		FCharacter *character = characterlist[op];
		character->setIsChatOp(true);
	}
	account->ui->setChatOperator(this, op, true);
}

COMMAND(DOP)
{
	(void)rawpacket;
	//Remove a character from the list of  chat operators.
	//DOP {"character": "Viona"}
	QString op = nodes.at("character").as_string().c_str();
	operatorlist.removeAll(op.toLower());

	if(isCharacterOnline(op)) {
		// Set flag in character
		FCharacter *character = characterlist[op];
		character->setIsChatOp(false);
	}
	account->ui->setChatOperator(this, op, false);
}

COMMAND(PIN)
{
	(void)rawpacket; (void)nodes;
	//debugMessage("Ping!");
	std::string cmd = "PIN";
	wsSend(cmd);
}