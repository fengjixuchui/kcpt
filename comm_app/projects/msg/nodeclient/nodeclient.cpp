/*
 * nodeclient.cpp
 *
 *  Created on: 2011-11-10
 *      Author: humingqing
 */

#include "nodeclient.h"
#include <netutil.h>
#include <waitgroup.h>
#include <tools.h>
#include <nodeparse.h>

CNodeClient::CNodeClient()
{
	_pEnv       = NULL ;
	_nodeid     = 0 ;
	_enable     = false ;
	_pAlloc     = new CAllocMsg;
	_pBuilder   = new CMsgBuilder( _pAlloc ) ;
	_pWaitQueue = new CWaitGroup( _pAlloc ) ;
}

CNodeClient::~CNodeClient()
{
	Stop() ;

	if ( _pWaitQueue != NULL ) {
		delete _pWaitQueue ;
		_pWaitQueue = NULL ;
	}

	if ( _pAlloc != NULL ) {
		delete _pAlloc ;
		_pAlloc = NULL ;
	}
}

// ��ʼ��
bool CNodeClient::Init( ISystemEnv *pEnv )
{
	_pEnv = pEnv ;

	//msg_node_id=100001
	//msg_node_server=127.0.0.1
	//msg_node_port=7555
	//msg_node_enable=0
	//msg_dev_name=eth0
	int nvalue = 0 ;
	if ( pEnv->GetInteger( "msg_node_enable" , nvalue ) ) {
		_enable = ( nvalue == 1 ) ;
	}
	if ( !_enable ) {
		OUT_INFO( NULL, 0, "NodeClient" , "msg node disable" ) ;
		return true;
	}

	// ȡ�ýڵ�ID��
	if ( ! pEnv->GetInteger( "msg_node_id" , nvalue ) ) {
		printf( "get msg node id failed\n" ) ;
		return false ;
	}
	_nodeid = nvalue ;

	// ȡ�÷�����IP�Ͷ˿�
	char buf[256] = {0} ;
	if ( ! pEnv->GetString( "msg_node_server" , buf ) ) {
		printf( "get msg node server failed\n" ) ;
		return false ;
	}
	_client_user._ip = buf ;

	// ȡ�ýڵ�������Ķ˿�
	if ( ! pEnv->GetInteger( "msg_node_port" , nvalue ) ) {
		printf( "get msg node port failed\n" ) ;
		return false ;
	}
	_client_user._port = nvalue ;
	_client_user._login_time = 0 ;

	// ȡ���豸������
	if ( !pEnv->GetString( "msg_dev_name" , buf ) ) {
		printf( "get msg dev name failed\n" ) ;
		return false ;
	}
	// ȡ�ñ�����IP�ĵ�ַ
	_msg_ip = netutil::addrToString( netutil::getLocalAddr( buf ) ) ;

	// ȡ��MSG�ļ����˿�
	if ( ! pEnv->GetInteger( "msg_listen_port" , nvalue ) ) {
		printf( "get msg listen port failed\n" ) ;
		return false ;
	}
	_msg_port = nvalue ;

	// ���ûص�����
	_pWaitQueue->SetNotify( this ) ;

	// ���÷ְ�����
	setpackspliter( &_pack_spliter ) ;

	return _pWaitQueue->Init() ;
}

// ��ʼ�߳�
bool CNodeClient::Start( void )
{
	if ( ! _enable )
		return true ;

	if ( ! StartClient( _client_user._ip.c_str() , _client_user._port , 1 ) ) {
		printf( "start node client failed\n" ) ;
		return false ;
	}
	return _pWaitQueue->Start() ;
}

// ֹͣ�߳�
void CNodeClient::Stop( void )
{
	if ( ! _enable )
		return ;

	StopClient() ;
	// ֹͣ�ȴ�����
	_pWaitQueue->Stop() ;
}

// ���ݵ���ʱ����
void CNodeClient::on_data_arrived( socket_t *sock, const void* data, int len)
{
	const char *ip      = sock->_szIp ;
	unsigned short port = sock->_port ;

	OUT_INFO( ip, port, "NodeClient" , "%s , length %d" , CNodeParser::Decode( (char*)data, len ) , len ) ;
	OUT_HEX( ip, port, NULL, (const char*)data , len ) ;

	if ( len < (int)sizeof(NodeHeader) ) {
		OUT_ERROR( ip, port , "NodeClient", "recv fd %d data len %d error" , sock->_fd, len ) ;
		return ;
	}

	NodeHeader *header = (NodeHeader *) (data) ;
	unsigned int mlen = ntohl( header->len ) ;
	// �������ݵ���ȷ��
	if ( (int)(mlen + sizeof(NodeHeader)) != len ) {
		OUT_ERROR( ip, port, "NodeClient", "recv fd %d data len %d error" , sock->_fd, len ) ;
		return ;
	}

	// ��¼���һ�λ�Ӧ��ʱ��
	_client_user._last_active_time = time(NULL) ;

	unsigned int   seq = ntohl( header->seq ) ;
	unsigned short cmd = ntohs( header->cmd ) ;
	switch( cmd ) {
	case NODE_CONNECT_RSP:
		{
			if ( mlen == sizeof(NodeLoginRsp) ) {
				NodeLoginRsp *rsp = (NodeLoginRsp*)( (char*)data + sizeof(NodeHeader) ) ;
				if ( rsp->result == 0 ) {
					_client_user._user_state = User::ON_LINE ;
				}
			} else {
				OUT_ERROR( ip, port, "NodeClient" , "NodeLoginRsp size error" ) ;
			}
		}
		break ;
	case NODE_DISCONN_RSP:
	case NODE_LINKTEST_RSP:
	case NODE_USERNAME_RSP:
	case NODE_GETMSG_RSP:
		break ;
	case NODE_USERNOTIFY_REQ:
		if ( mlen > sizeof(NodeUserNotify) ) {
			char *p =  (char*)data + sizeof(NodeHeader) ;
			// ���������û�
			NodeUserNotify *body = ( NodeUserNotify *) p ;
			unsigned short num = ntohs( body->num ) ;

			unsigned short result = 0 ;
			if ( (unsigned int)( num * sizeof(UserInfo) + sizeof(NodeUserNotify) ) == mlen ) {
				p = p + sizeof(NodeUserNotify) ;

				char user[13] = {0} ;
				char pwd[9]   = {0} ;

 				// ����MSG���û�
				for ( unsigned short i = 0; i < num ; ++ i ) {
					UserInfo *info = ( UserInfo *) p ;

					safe_memncpy( user, info->user, sizeof(info->user) ) ;
					safe_memncpy( pwd , info->pwd , sizeof(info->pwd ) ) ;

					OUT_INFO( ip, port, "NodeClient" , "add user name %s pwd %s" , user, pwd ) ;

					_pEnv->GetMsgClientServer()->AddNodeUser( user, pwd ) ;

					p = p + sizeof(UserInfo) ;

					++ result ;
				}
			}

			DataBuffer buf ;
			_pBuilder->BuildUserNotifyResp( buf , seq, result ) ;
			// ������Ӧ����
			if ( ! SendDataEx( sock, buf.getBuffer(), buf.getLength() ) ) {
				OUT_ERROR( ip, port, "NodeClient" , "send data fd %d seq %04x response failed" , sock->_fd, seq ) ;
			}
		}
		break ;
	case NODE_MSGERROR_REQ:
	case NODE_MSGCHG_REQ:
		break ;
	}

	// �����ȴ���Ӧ������
	if ( cmd & 0x8000 ) {
		// ���Ϊ��Ӧ��Ҫ�����ȴ�����
		_pWaitQueue->DelQueue( seq, sock, true ) ;
	}
}

// �Ͽ����Ӵ���
void CNodeClient::on_dis_connection( socket_t *sock )
{
	OUT_WARNING( sock->_szIp , sock->_port, _client_user._user_id.c_str(), "Disconnection fd %d" , sock->_fd );
	_client_user._user_state = User::OFF_LINE ;
}

// ��ʱ�߳�
void CNodeClient::TimeWork()
{
	while(1) {
		if ( ! Check() ) break ;

		time_t now = time(NULL) ;
		if ( _client_user._user_state == User::OFF_LINE ) {
			if( now- _client_user._login_time > 60 ) {
				ConnectServer( _client_user, 60 ) ;
			}
		} else if ( _client_user._user_state == User::ON_LINE ) {
			// ��������
			if ( now - _client_user._last_active_time > 180 ) {
				// �Ͽ���������
				if ( _client_user._fd != NULL ) {
					CloseSocket( _client_user._fd ) ;
				}
				// ����
				ConnectServer( _client_user , 60 ) ;

			}else if ( now - _client_user._last_active_time > 30 ) {
				// ��������������·
				SendLinkTest() ;
			}
		}
		//����ط�����ʱ��ͬ��������
		sleep(5);
	}
}

// �����߳�
void CNodeClient::NoopWork()
{

}

// ������½����Ϣ
int CNodeClient::build_login_msg( User &user, char *buf, int buf_len )
{
	AddrInfo addr ;
	safe_memncpy( addr.ip, _msg_ip.c_str(), sizeof(addr.ip) ) ;
	addr.port = _msg_port ;

	MsgData *msg = _pBuilder->BuildLoginReq( _nodeid, FD_NODE_MSG , addr ) ;

	DataBuffer mbuf ;
	_pBuilder->BuildMsgBuffer( mbuf, msg ) ;
	_pWaitQueue->AddGroup( _client_user._fd, msg->seq, msg ) ;

	memcpy( buf, mbuf.getBuffer(), mbuf.getLength() ) ;

	return mbuf.getLength() ;
}

// ������������
void CNodeClient::SendLinkTest( void )
{
	// ȡ���û�����ѹ��
	int ncar = _pEnv->GetMsgClientServer()->GetOnlineSize() ;

	MsgData *msg = _pBuilder->BuildLinkTestReq( ncar ) ;

	DataBuffer buf ;
	_pBuilder->BuildMsgBuffer( buf, msg ) ;
	_pWaitQueue->AddGroup( _client_user._fd, msg->seq, msg ) ;

	if ( ! SendDataEx( _client_user._fd, buf.getBuffer(), buf.getLength() ) ) {
		OUT_ERROR( NULL, 0, NULL, "SendLinkTest fd %d failed" , _client_user._fd ) ;
		_pWaitQueue->DelQueue( msg->seq,  _client_user._fd, true ) ;
		return ;
	}
}

void CNodeClient::NotifyMsgData( socket_t *sock, MsgData *p , ListFd &fds, unsigned int op )
{
	// ���������Ӧ��ʱ˵���ڵ��쳣ֱ�Ӷ���
	OUT_INFO( NULL, 0, "NodeClient" , "%s seq %d, fd %d cmd %04x length %d" ,
				(op == MSG_TIMEOUT) ? "timeout" : "success" , p->seq, sock->_fd, p->cmd, p->buf.getLength() ) ;
}

// ���ط������ݺ���
bool CNodeClient::SendDataEx( socket_t *sock, const char *data, int len )
{
	if ( sock == NULL )
		return false ;

	if ( ! SendData( sock, data, len ) ) {
		OUT_ERROR( sock->_szIp, sock->_port, "Send" , "send data error fd %d" , sock->_fd ) ;
		OUT_HEX( sock->_szIp, sock->_port, "Send", data, len ) ;
		return false ;
	}
	OUT_SEND( sock->_szIp, sock->_port, "Send" , "%s , fd %d, length %d" , CNodeParser::Decode( data, len ), sock->_fd, len ) ;
	OUT_HEX( sock->_szIp, sock->_port, "Send", data, len ) ;

	return true ;
}