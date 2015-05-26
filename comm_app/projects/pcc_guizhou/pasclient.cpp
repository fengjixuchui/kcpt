#include "pasclient.h"
#include <ProtoHeader.h>
#include <Base64.h>
#include "pccutil.h"
#include <crc16.h>
#include <comlog.h>
#include <tools.h>
#include "pconvert.h"
#include <BaseTools.h>

PasClient::PasClient(): _macid2seqid(true), _statinfo("PasClient")
{
	_last_handle_user_time = time(NULL) ;
	_down_port = 0 ;
}

PasClient::~PasClient()
{
	Stop() ;
}

bool PasClient::Init( ISystemEnv *pEnv )
{
	_pEnv = pEnv ;

	char value[1024] = {0} ;
	if ( _pEnv->GetString("pcc_down_ip", value) ){
		_down_ip = value ;
	}
	// ȡ���Զ�Ӧ���ƽ̨��ڵ�����
	if ( _pEnv->GetString( "pcc_postquery", value ) ) {
		_postpath = value ;
	}

	int port = 0 ;
	if ( _pEnv->GetInteger("pcc_listen_port", port ) ){
		_down_port = port ;
	}

	// �����û�����ص�����
	_pEnv->SetNotify( PAS_USER_TAG , this ) ;

	return true ;
}

void PasClient::Stop( void )
{
	StopClient() ;
}

bool PasClient::Start( void )
{
	return StartClient( "0.0.0.0", 0, 3 ) ;
}

bool PasClient::ConnectServer(User &user, int timeout /*= 10*/)
{
	if(time(0) - user._connect_info.last_reconnect_time < user._connect_info.timeval)
			return false;

	bool ret = false;
	if (user._fd  != NULL)
	{
		OUT_WARNING( user._ip.c_str(), user._port ,NULL,"fd %d close socket", user._fd->_fd );
		CloseSocket(user._fd);
	}
	user._fd = _tcp_handle.connect_nonb(user._ip.c_str(), user._port, timeout);
	ret = (user._fd != NULL) ? true:false;

	user._last_active_time = time(0);
	user._login_time       = time(0);
	user._connect_info.last_reconnect_time = time(0);

	if(ret )
	{
		user._user_state = User::WAITING_RESP;

		// ����Ӻ�������
		_online_user.AddUser( user._user_id, user ) ;

		UpConnectReq req;
		req.header.msg_seq 		= ntouv32(_proto_parse.get_next_seq());
		req.header.access_code  = ntouv32( user._access_code);
		req.user_id        		= ntouv32( atoi(user._user_name.c_str()) ) ;
		memcpy( req.password , user._user_pwd.c_str(), sizeof(req.password) ) ;

		// ����������о���Ҫ��������
		if ( _down_port > 0 ) {
			safe_memncpy( (char*)req.down_link_ip, _down_ip.c_str(), sizeof(req.down_link_ip) ) ;
			req.down_link_port = ntouv16( _down_port ) ;
		}
		OUT_INFO(user._ip.c_str(),user._port,user._user_id.c_str(),"Send UpConnectReq,down-link state:CONNECT_WAITING_RESP");
		SendCrcData( user._fd , (const char*)&req, sizeof(req) );
	}
	else
	{
		user._user_state = User::OFF_LINE;
	}

	/**
	if(user._connect_info.keep_alive == ReConnTimes)
		user._connect_info.reconnect_times--;
	*/
	return ret;
}

void PasClient::on_data_arrived( socket_t *sock, const void* data, int len)
{
	C5BCoder coder ;
	if ( ! coder.Decode( (const char *)data, len ) )
	{
		OUT_WARNING(sock->_szIp, sock->_port, NULL,"Except packet header or tail");
		return;
	}

	// ����ӽ�������
	EncryptData( (unsigned char*) coder.GetData() , (unsigned int)coder.GetSize() , false ) ;
	// ��������
	HandleOnePacket( sock,(const char*)coder.GetData() , coder.GetSize() );
}

// �������ŵ�����ʡƽ̨DOWN������
void PasClient::HandlePasDownData( const int access, const char *data, int len )
{
	User user = _online_user.GetUserByAccessCode( access ) ;
	if ( user._user_id.empty() || user._user_state != User::ON_LINE ) {
		OUT_WARNING( user._ip.c_str(), user._port, user._user_id.c_str(), "HandlePasDownData user not online" ) ;
		return ;
	}
	// �����������·��������
	HandleOnePacket( user._fd , data , len ) ;
}

void PasClient::on_dis_connection( socket_t *sock )
{
	//ר�Ŵ���ײ����·ͻȻ�Ͽ��������������ʱ�����������µĶϿ������
	User user = _online_user.GetUserBySocket( sock );
	if ( ! user._user_id.empty() || user._fd == NULL ) {
		// �������״̬����
		//_srvCaller.updateConnectState( UP_DISCONNECT_RSP , _pEnv->GetSequeue() , GetAreaCode(user) , CONN_MASTER , CONN_DISCONN ) ;
	}
	if ( user._user_state != User::DISABLED ) {
		OUT_WARNING( sock->_szIp, sock->_port, user._user_id.c_str(), "Disconnection fd %d", sock->_fd );
		user._user_state = User::OFF_LINE ;
	}
	user._fd = NULL ;
	_online_user.SetUser( user._user_id, user ) ;
}

void PasClient::TimeWork()
{
	/*
	 * 1.����ʱ������ȥ����
	 * 2.��ʱ����NOOP��Ϣ
	 * 3.Reload�����ļ��е��µ����ӡ�
	 * 4.
	 */
	while(1) {
		if ( ! Check() ) break ;
		// ��������
		HandleOfflineUsers() ;

		// ���û���ʮ���ӳ�ʱ
		_pEnv->GetMsgCache()->CheckData( 600 ) ;
		// ����ӳ�ʱʱ��
		_macid2seqid.CheckTimeOut( 300 ) ;

		sleep(2) ;
	}
}

void PasClient::NoopWork()
{
	while(1)
	{
		// ��������û�����������
		HandleOnlineUsers( 30 ) ;
		// ��ӡͳ����Ϣ����
		_statinfo.Check() ;

		sleep(3) ;
	}
}

// ��PAS������ͨ��������
void PasClient::HandlePasUpData( const int access, const char *data, int len )
{
	User user = _online_user.GetUserByAccessCode( access ) ;
	if ( user._user_id.empty() ) {
		OUT_WARNING( user._ip.c_str(), user._port, user._user_id.c_str(), "HandlePasDownData user empty" ) ;
		return ;
	}

	// �û�û�����ߵ����
	if ( user._user_state != User::ON_LINE ) {
		OUT_WARNING( user._ip.c_str(), user._port, user._user_id.c_str(), "HandlePasDownData user not online" ) ;
		return ;
	}

	Header *header = ( Header *) data ;
	header->access_code = ntouv32( user._access_code ) ;
	// ���������������ѭ����Ĵ���
	if ( ! SendCrcData( user._fd, data, len ) ) {
		// Todo: Send failed
	}
}

void PasClient::HandleClientData( const char *code, const char *data, int len )
{
	if ( SendDataToUser( code, data, len ) ) {
		OUT_SEND( NULL, 0, code, "Send data %s", _proto_parse.Decoder(data, len).c_str() ) ;
	} else {
		OUT_ERROR( NULL, 0, code, "Send Data %s Failed", _proto_parse.Decoder(data, len).c_str() ) ;
	}
}

bool PasClient::SendDataToUser( const string &area_code, const char *data, int len)
{
	char buf[512] = {0};
	sprintf( buf, "%s%s", PAS_USER_TAG, area_code.c_str() ) ;

	User user = _online_user.GetUserByUserId(buf);
	if (user._user_id.empty()) {
		return false;
	}

	// �û�û�����ߵ����
	if ( user._user_state != User::ON_LINE ) {
		OUT_WARNING( user._ip.c_str(), user._port, buf, "HandlePasDownData user not online" ) ;
		return false;
	}

	Header *header = ( Header *) data ;
	header->access_code = ntouv32( user._access_code ) ;

	// ���Ϊ��չ��Ϣ����ӳ���ͳ���д���
	if ( ntouv16(header->msg_type) == UP_EXG_MSG ) {
		char szmacid[128] = {0} ;
		ExgMsgHeader *msgheader = (ExgMsgHeader*) (data + sizeof(Header));
		sprintf( szmacid, "%d_%s", msgheader->vehicle_color, (const char *) msgheader->vehicle_no ) ;
		_statinfo.AddVechile( user._access_code, szmacid, STAT_SEND ) ;
	}

	// ���������������ѭ����Ĵ���
	return SendCrcData( user._fd, data, len ) ;
}

void PasClient::HandleOfflineUsers()
{
	vector<User> vec_users = _online_user.GetOfflineUsers(3*60);
	for(int i = 0; i < (int)vec_users.size(); i++) {
		User &user = vec_users[i];
		if(user._socket_type == User::TcpClient){
			if(user._fd != NULL){
				OUT_WARNING( user._ip.c_str() , user._port , user._user_id.c_str() ,
						"HandleOfflineUsers PasClient TcpClient close socket fd %d", user._fd->_fd );
				CloseSocket(user._fd);
			}
		} else if(user._socket_type == User::TcpConnClient) {
			if(user._fd !=NULL ){
				OUT_INFO( user._ip.c_str() , user._port , user._user_id.c_str() ,
						"HandleOfflineUsers PasClient TcpConnClient close socket fd %d", user._fd->_fd );
				user.show();
				CloseSocket(user._fd);
				user._fd = NULL;
			}
			if ( ! ConnectServer(user, 10) ) {
				//����ʧ�ܣ�һ����Ҫ����
				_online_user.AddUser( user._user_id, user ) ;
			}
		}
	}
}

void PasClient::HandleOnlineUsers(int timeval)
{
	time_t now = time(NULL) ;
	if( now - _last_handle_user_time < timeval){
		return;
	}
	_last_handle_user_time = now ;

	vector<User> vec_users = _online_user.GetOnlineUsers();
	for(int i = 0; i < (int)vec_users.size(); i++)
	{
		User &user = vec_users[i] ;
		if( user._socket_type == User::TcpConnClient && user._fd != NULL ) {
			UpLinkTestReq req;
			req.header.access_code = ntouv32(user._access_code);
			req.header.msg_seq 	   = ntouv32(_proto_parse.get_next_seq());
			req.crc_code 		   = ntouv16(GetCrcCode((const char*)&req,sizeof(req)));
			Send5BCodeData( user._fd,(const char*)&req,sizeof(req), false );

			OUT_SEND( user._ip.c_str(), user._port, user._user_id.c_str(),
					"%s", _proto_parse.Decoder((const char*)&req,sizeof(req)).c_str());
		}
	}
}

// �������ݽ���5B���봦��
bool PasClient::Send5BCodeData( socket_t *sock, const char *data, int len  , bool bflush )
{
	if ( sock == NULL )  {
		return false ;
	}
	C5BCoder  coder;
	if ( ! coder.Encode( data, len ) ){
		OUT_ERROR( sock->_szIp, sock->_port, NULL, "Send5BCodeData failed , socket fd %d", sock->_fd ) ;
		return false ;
	}

	OUT_HEX(sock->_szIp, sock->_port, "SEND", coder.GetData(), coder.GetSize());

	return SendData( sock, coder.GetData(), coder.GetSize() ) ;
}

// �������´���ѭ���������
bool PasClient::SendCrcData( socket_t *sock, const char* data, int len)
{
	// ����ѭ����
	char *buf = new char[len+1] ;
	memcpy( buf, data, len ) ;
	// ����ӽ�������
	EncryptData( (unsigned char*) buf , len , true ) ;
	// ͳһ����ѭ�������֤
	unsigned short crc_code = ntouv16( GetCrcCode( buf, len ) ) ;
	unsigned int   offset   = len - sizeof(Footer) ;
	// �滻ѭ�����ڴ��λ������
	memcpy( buf + offset , &crc_code, sizeof(short) ) ;

	bool bSend = Send5BCodeData( sock, buf , len ) ;

	delete [] buf ;

	return bSend ;
}

void PasClient::HandleOnePacket( socket_t *sock, const char* data , int len )
{
	const char *ip = sock->_szIp ;
	unsigned short port = sock->_port ;

	if(  len < (int)sizeof(Header) || data == NULL ){
		OUT_ERROR( ip, port, NULL, "data length errro length %d", len ) ;
		OUT_HEX( ip, port, NULL , data, len ) ;
		return ;
	}

	Header *header = (Header *) data;
	unsigned int access_code = ntouv32(header->access_code);
	string str_access_code   = uitodecstr(access_code);
	unsigned int msg_len     = ntouv32(header->msg_len);
	unsigned short msg_type  = ntouv16(header->msg_type);
	string mac_id = _proto_parse.GetMacId( data , len );

	// ��ӽ��յ�������ͳ�Ʒ���
	_statinfo.AddRecv( access_code ) ;

	if ( msg_len > len || msg_len == 0 ) {
		OUT_ERROR( ip, port, NULL, "data length errro length %d , msg len %d", len , msg_len ) ;
		OUT_HEX( ip, port, NULL , data, len ) ;
		return ;
	}

	OUT_RECV( ip, port, str_access_code.c_str(), "%s", _proto_parse.Decoder(data,len).c_str() ) ;
	OUT_HEX( ip, port, str_access_code.c_str(), data, len ) ;

	User user = _online_user.GetUserBySocket( sock ) ;

	if (msg_type == UP_CONNECT_RSP)
	{
		UpConnectRsp *rsp = ( UpConnectRsp *) data ;
		switch(rsp->result)
		{
		case 0:
			OUT_INFO(ip,port,str_access_code.c_str(),"login check success,access_code:%d  up-link ON_LINE",access_code);
			user._user_state  = User::ON_LINE ;
			break;
		case 1:
			OUT_WARNING(ip,port,str_access_code.c_str(),"login check fail,ip is invalid");
			break;
		case 2:
			OUT_WARNING(ip,port,str_access_code.c_str(),"login check fail,accesscode is invalid,close it");
			break;
		case 3:
			OUT_WARNING(ip,port,str_access_code.c_str(),"login check fail,user_name:%s is invalid,close it", user._user_name.c_str());
			break;
		case 4:
			OUT_WARNING(ip,port,str_access_code.c_str(),"login check fail,user_password:%s is invalid,close it",user._user_pwd.c_str());
			break;
		default:
			OUT_WARNING(ip,port,str_access_code.c_str(),"login check fail,other error,close it");
			break;
		}

		if ( rsp->result != 0 ) {
			CloseSocket( sock ) ;
			return ;
		}

		// ������
		user._last_active_time = time(NULL) ;
		// �����û���̬
		_online_user.SetUser( user._user_id, user ) ;
		// ��������״̬����
		//_srvCaller.updateConnectState( UP_CONNECT_RSP , _pEnv->GetSequeue() , areacode, CONN_MASTER , CONN_CONNECT ) ;

		return ;
	}
	else if (msg_type == UP_LINKTEST_RSP)//"NOOP_ACK")
	{
	}
	else if ( msg_type == UP_DISCONNECT_RSP ) // �յ��Ͽ�������Ӧ��ֱ�Ӵ�������״̬
	{
		// ������������������յ����������Ӧ���ٶϿ�����
		if ( User::DISABLED == user._user_state ) {
			if ( user._fd != NULL ) {
				OUT_ERROR( ip, port, str_access_code.c_str(), "disconnect response fd %d" , user._fd->_fd ) ;
				// �ر�����
				CloseSocket( user._fd ) ;
			}
		} else {
			user._user_state = User::OFF_LINE ;
			// ���͹رմ���·���������Ƿ��쳣�����������������ע��
			_pEnv->GetPccServer()->Close( access_code , UP_DISCONNECT_INFORM , 0x00 ) ;
		}
	}
	else if ( msg_type == DOWN_DISCONNECT_INFORM )   // ����·�·�����·�Ͽ�֪ͨ
	{
		// �����رմ���·����
		// _pEnv->GetPccServer()->Close( user._access_code, 0, 0 ) ;
		OUT_INFO( ip, port, str_access_code.c_str(), "Recv DOWN_DISCONNECT_INFORM" ) ;
	}
	else if ( msg_type == DOWN_CLOSELINK_INFORM )  // �����ر�������·֪ͨ
	{
		// �����ر�������·����
		// _pEnv->GetPccServer()->Close( user._access_code, 0, 0 ) ;
		OUT_INFO( ip, port, str_access_code.c_str(), "Recv DOWN_CLOSELINK_INFORM" ) ;
	}
	else if ( msg_type == DOWN_PLATFORM_MSG )
	{
		DownPlatformMsg *plat_msg = ( DownPlatformMsg * ) ( data + sizeof(Header) ) ;
		unsigned short data_type = ntouv16( plat_msg->data_type ) ;
		switch( data_type ) {
		case DOWN_PLATFORM_MSG_POST_QUERY_REQ:  // ƽ̨�����Ϣ
			{
				if ( len < (int)sizeof(DownPlatformMsgPostQueryReq) ) {
					OUT_ERROR( ip,port,str_access_code.c_str(), "DOWN_PLATFORM_MSG_POST_QUERY_REQ data length error , length %d" , len ) ;
					return ;
				}
				DownPlatformMsgPostQueryReq *msg = (DownPlatformMsgPostQueryReq*)data;

				// ȡ��ƽ̨��ڵĳ���
				int nlen = ntouv32( msg->down_platform_body.info_length ) ;
				if ( nlen < 0 || nlen + (int)sizeof(DownPlatformMsgPostQueryReq) > len ) {
					OUT_ERROR( ip,port,str_access_code.c_str(), "DOWN_PLATFORM_MSG_POST_QUERY_REQ data length error , length %d, content len %d" , len , nlen ) ;
					return ;
				}

				UpPlatformMsgPostQueryAck resp;

				resp.header.msg_len               = 0 ;  // ��ʱȡ0�����յ���Ӧ�������¼���
				resp.header.access_code 	      = ntouv32( user._access_code ) ;
				resp.header.msg_seq 		      = msg->header.msg_seq ;
				resp.up_platform_msg.data_length  = 0 ;
				resp.up_platform_post.info_id	  = msg->down_platform_body.info_id ;
				resp.up_platform_post.msg_len	  = 0 ;
				resp.up_platform_post.object_type = msg->down_platform_body.object_type ;
				safe_memncpy( resp.up_platform_post.object_id, msg->down_platform_body.object_id , sizeof(resp.up_platform_post.object_id) ) ;

				std::string text ;

				CQString content;
				content.SetString((const char*) (data + sizeof(DownPlatformMsgPostQueryReq)), nlen);

				CBase64 base64;
				base64.Encode(content.GetBuffer(), content.GetLength());

				unsigned int seqid = _pEnv->GetSequeue();
				char szKey[256] = { 0 };
				_pEnv->GetCacheKey(seqid, szKey);

				// ��ӵ��ȴ�������
				_pEnv->GetMsgCache()->AddData(szKey, (const char *) &resp, sizeof(resp));

				char messageId[512] = { 0 };
				sprintf(messageId, "%u", ntouv32( msg->down_platform_body.info_id ));

				char objectType[128] = { 0 };
				sprintf(objectType, "%u", msg->down_platform_body.object_type);

				char objectId[256] = { 0 };
				safe_memncpy(objectId, msg->down_platform_body.object_id, sizeof(msg->down_platform_body.object_id));

				char areaId[128] = { 0 };
				sprintf(areaId, "%u", GetAreaCode(user));

				char macid[64];
				snprintf(macid, 64, "%u_%s", user._access_code, areaId);

				//CAITS 1_2_3 11000020_430000 4 L_PLAT {TYPE:D_PLAT,PLATQUERY:2|500105010649|147258369|MSszPT8=}

				string inner = "CAITS " + string(szKey) + " " + string(macid) + " 4 L_PLAT {TYPE:D_PLAT,PLATQUERY";
				inner += ":" + string(objectType);
				inner += "|" + string(objectId);
				inner += "|" + string(messageId);
				inner += "|" + string(base64.GetBuffer());
				inner += "} \r\n";

				// ����ƽ̨���
				//_srvCaller.addForMsgPost( DOWN_PLATFORM_MSG_POST_QUERY_REQ , seqid, content.GetBuffer() , messageId , objectId, objectType , areaId ) ;
				((IMsgClient*) _pEnv->GetMsgClient())->HandleUpMsgData(SEND_ALL, inner.c_str(), inner.length());

				OUT_PRINT( ip, port , str_access_code.c_str(), "DOWN_PLATFORM_MSG_POST_QUERY_REQ seqid %u, messageId %s, objectId %s, objectType %s, areaId %s, content %s, key: %s" ,
						seqid , messageId, objectId, objectType, areaId , content.GetBuffer(), szKey );
			}
			break ;
		case DOWN_PLATFORM_MSG_INFO_REQ:
			{
				if ( len < (int)sizeof(DownPlatformMsgInfoReq) ) {
					OUT_ERROR( ip,port,str_access_code.c_str(), "DOWN_PLATFORM_MSG_INFO_REQ data length error , length %d" , len ) ;
					return ;
				}
				//ƽ̨�·���������
				DownPlatformMsgInfoReq * msg = ( DownPlatformMsgInfoReq *) data ;

				// ȡ��ƽ̨��ڵĳ���
				int nlen = ntouv32( msg->info_length ) ;
				if ( nlen < 0 || nlen + (int)sizeof(DownPlatformMsgInfoReq) > len ) {
					OUT_ERROR( ip,port,str_access_code.c_str(), "DOWN_PLATFORM_MSG_INFO_REQ data length error , length %d, content len %d" , len , nlen ) ;
					return ;
				}

				CQString content ;
				content.SetString( (const char*)(data+sizeof(DownPlatformMsgInfoReq)) , nlen ) ;

				CBase64 base64;
				base64.Encode(content.GetBuffer(), content.GetLength());

				unsigned int seqid = _pEnv->GetSequeue() ;
				char szKey[256]={0};
				_pEnv->GetCacheKey( seqid, szKey ) ;

				UpPlatFormMsgInfoAck resp;

				resp.header.msg_len               = ntouv32( sizeof(resp) ) ;
				resp.header.msg_type			  = ntouv16( UP_PLATFORM_MSG ) ;
				resp.header.access_code 	      = ntouv32( user._access_code ) ;
				resp.header.msg_seq 		      = msg->header.msg_seq ;
				resp.up_platform_msg.data_type	  = ntouv16( UP_PLATFORM_MSG_INFO_ACK ) ;
				resp.up_platform_msg.data_length  = ntouv32( sizeof(int) ) ;
				resp.info_id 					  = msg->info_id ;

				// ��ӵ��ȴ�������
				//_pEnv->GetMsgCache()->AddData( szKey, (const char *)&resp, sizeof(resp) ) ;

				char messageId[512] = {0} ;
				sprintf( messageId, "%d" , ntouv32(  msg->info_id ) ) ;

				char objectType[128] = {0} ;
				sprintf( objectType, "%d", msg->object_type ) ;

				char objectId[256] = {0} ;
				safe_memncpy( objectId, msg->object_id , sizeof(msg->object_id) ) ;

				char areaId[128] = {0} ;
				sprintf( areaId, "%u" , GetAreaCode(user) ) ;

				char macid[64];
				snprintf(macid, 64, "%u_%s", user._access_code, areaId);

				string inner = "CAITS " + string(szKey) + " " + string(macid) + " 4 L_PLAT {TYPE:D_PLAT,PLATMSG";
				inner += ":" + string(objectType);
				inner += "|" + string(objectId);
				inner += "|" + string(messageId);
				inner += "|" + string(base64.GetBuffer());
				inner += "} \r\n";

				// ����ƽ̨���
				//_srvCaller.addForMsgInfo( DOWN_PLATFORM_MSG_INFO_REQ , seqid, content.GetBuffer() , messageId , objectId, objectType , areaId ) ;
				((IMsgClient*)_pEnv->GetMsgClient())->HandleUpMsgData(SEND_ALL, inner.c_str(), inner.length() ) ;

				HandlePasUpData(access_code, (char*)&resp, sizeof(UpPlatFormMsgInfoAck));

				OUT_PRINT( ip, port , str_access_code.c_str(), "DOWN_PLATFORM_MSG_INFO_REQ seqid %u, messageId %s, objectId %s, objectType %s, areaId %s, content %s" ,
										seqid , messageId, objectId, objectType, areaId , content.GetBuffer() ) ;
			}
			break ;
		}
	}
	else if ( msg_type == DOWN_CTRL_MSG )
	{
		if ( len < (int)sizeof(DownCtrlMsgHeader) ){
			OUT_ERROR( ip,port,str_access_code.c_str(), "DOWN_CTRL_MSG data length error , length %d" , len ) ;
			return ;
		}

		DownCtrlMsgHeader *req = ( DownCtrlMsgHeader *) data ;
		int data_type = ntouv16( req->ctrl_msg_header.data_type ) ;

		char carnum[128]= {0};
		safe_memncpy( carnum, req->ctrl_msg_header.vehicle_no, sizeof(req->ctrl_msg_header.vehicle_no) ) ;
		char carcolor[128] = {0} ;
		sprintf( carcolor, "%d" , req->ctrl_msg_header.vehicle_color ) ;

		char macidSrc[32];
		snprintf(macidSrc, 32, "%s_%s", carcolor, carnum);

		string macidDst = "";
		if( ! _pEnv->GetRedisCache()->HGet("KCTX.PLATE2SIM", macidSrc, macidDst) || macidDst.empty()) {
			OUT_WARNING(user._ip.c_str(), user._port, user._user_name.c_str(), "KCTX.PLATE2SIM: %s not exist", macidSrc);
			return;
		}

		switch( data_type )
		{
		case DOWN_CTRL_MSG_MONITOR_VEHICLE_REQ: //����· ������
			{
				if ( len < (int)sizeof(DownCtrlMsgMonitorVehicleReq) ){
					OUT_ERROR( ip,port,str_access_code.c_str(), "DOWN_CTRL_MSG_MONITOR_VEHICLE_REQ data length %d error" , len ) ;
					return ;
				}

				DownCtrlMsgMonitorVehicleReq *msg = (DownCtrlMsgMonitorVehicleReq *) data;

				unsigned int seqid = _pEnv->GetSequeue() ;

				char szKey[256]={0};
				_pEnv->GetCacheKey( seqid, szKey ) ;

				char szphone[128] = {0};
				safenumber( szphone, msg->monitor_tel, sizeof(msg->monitor_tel) ) ;

				// �����ݻ��棬Ȼ��Ҫ���ݻ������ݶ�Ӧ����
				UpCtrlMsgMonitorVehicleAck resp;//ack Ӧ��ͨ������·�ظ�
				resp.header.msg_len 	= ntouv32(sizeof(UpCtrlMsgMonitorVehicleAck));
				resp.header.access_code = ntouv32( user._access_code ) ;
				resp.header.msg_seq 	= msg->header.msg_seq ;
				resp.ctrl_msg_header.vehicle_color = msg->ctrl_msg_header.vehicle_color;
				safe_memncpy(resp.ctrl_msg_header.vehicle_no, msg->ctrl_msg_header.vehicle_no, sizeof(resp.ctrl_msg_header.vehicle_no) );
				resp.ctrl_msg_header.data_length = ntouv32( sizeof(unsigned char) ) ;
				resp.result 			= 0x00;

				// ��ӵ����������
				_pEnv->GetMsgCache()->AddData( szKey, (const char *)&resp, sizeof(resp) ) ;

				int szlen = 0;
				char szbuf[1024] = {0} ;
				szlen += snprintf(szbuf + szlen, 1024 - szlen, "CAITS %s %s", szKey, macidDst.c_str());
				szlen += snprintf(szbuf + szlen, 1024 - szlen, " 4 D_CTLM {TYPE:9,RETRY:0,VALUE:%s} \r\n" , szphone ) ;

				((IMsgClient*)_pEnv->GetMsgClient())->HandleUpMsgData( macidDst.c_str(), szbuf, szlen) ;
				// ����HTTP��������
				//_srvCaller.getTernimalByVehicleByTypeEx( DOWN_CTRL_MSG_MONITOR_VEHICLE_REQ, seqid, carnum , carcolor , szbuf ) ;

				OUT_PRINT( ip, port , str_access_code.c_str(), "DOWN_CTRL_MSG_MONITOR_VEHICLE_REQ seqid %u, carnum %s, carcolor %s" , seqid , carnum, carcolor ) ;
			}
			break;
		case DOWN_CTRL_MSG_TAKE_PHOTO_REQ:  // ��������
			{
				if ( len < (int)sizeof(DownCtrlMsgTakePhotoReq) ) {
					OUT_ERROR( ip,port,str_access_code.c_str(), "DOWN_CTRL_MSG_TAKE_PHOTO_REQ data length %d error" , len ) ;
					return ;
				}

				DownCtrlMsgTakePhotoReq *msg = ( DownCtrlMsgTakePhotoReq *) data ;

				char szKey[256]={0};
				unsigned int seqid = _pEnv->GetSequeue() ;
				_pEnv->GetCacheKey( seqid, szKey ) ;

				// ����ͷͨ��ID|��������|¼��ʱ��|�����־|�ֱ���|��Ƭ����|����|�Աȶ�|���Ͷ�|ɫ��
				int szlen = 0;
				char szbuf[1024] = {0} ;
				szlen += snprintf(szbuf + szlen, 1024 - szlen, "CAITS %s %s", szKey, macidDst.c_str());
				szlen += snprintf(szbuf + szlen, 1024 - szlen, " 4 D_CTLM {TYPE:10,RETRY:0,VALUE:"
						"%d|1|1|0|%d|10|128|128|128|128} \r\n", msg->lens_id, msg->size - 1);

				((IMsgClient*)_pEnv->GetMsgClient())->HandleUpMsgData( macidDst.c_str(), szbuf, szlen) ;

				//_srvCaller.getTernimalByVehicleByTypeEx( DOWN_CTRL_MSG_TAKE_PHOTO_REQ, seqid, carnum , carcolor, szbuf ) ;

				OUT_PRINT( ip, port , str_access_code.c_str(), "DOWN_CTRL_MSG_TAKE_PHOTO_REQ seqid %u, carnum %s, carcolor %s" , seqid , carnum, carcolor ) ;
			}
			break ;
		case DOWN_CTRL_MSG_TEXT_INFO:  // �·��ı�
			{
				if ( len < (int)sizeof(DownCtrlMsgTextInfoHeader) ) {
					OUT_ERROR( ip, port, str_access_code.c_str(), "DOWN_CTRL_MSG_TEXT_INFO data length %d error" , len ) ;
					return  ;
				}

				DownCtrlMsgTextInfoHeader *msg = ( DownCtrlMsgTextInfoHeader *)data ;

				unsigned int seqid = _pEnv->GetSequeue() ;
				char szKey[256]={0};
				_pEnv->GetCacheKey( seqid, szKey ) ;

				CBase64 base;
				int nlen = ntouv32( msg->msg_len ) ;
				base.Encode( data + sizeof(DownCtrlMsgTextInfoHeader) , nlen ) ;

				UpCtrlMsgTextInfoAck resp ;
				resp.header.msg_len 	= ntouv32(sizeof(UpCtrlMsgTextInfoAck));
				resp.header.access_code = ntouv32( user._access_code ) ;
				resp.header.msg_seq 	= msg->header.msg_seq ;
				resp.ctrl_msg_header.vehicle_color = msg->ctrl_msg_header.vehicle_color;
				safe_memncpy(resp.ctrl_msg_header.vehicle_no, msg->ctrl_msg_header.vehicle_no, sizeof(resp.ctrl_msg_header.vehicle_no) );
				resp.ctrl_msg_header.data_length = ntouv32( sizeof(unsigned char) + sizeof(int) ) ;
				resp.msg_id				= msg->msg_sequence ;
				resp.result 			= 0x00;

				//��ӵ����������
				_pEnv->GetMsgCache()->AddData( szKey , (const char *)&resp, sizeof(resp));
				OUT_INFO( ip, port, str_access_code.c_str() , "Add UpCtrlmsgTexInfoAck key %s", szKey ) ;

				int szlen = 0;
				char szbuf[1024] = {0} ;
				szlen += snprintf(szbuf + szlen, 1024 - szlen, "CAITS %s %s", szKey, macidDst.c_str());
				szlen += snprintf(szbuf + szlen, 1024 - szlen, " 4 D_SNDM {TYPE:1,1:255,2:%s} \r\n",  base.GetBuffer());

				((IMsgClient*)_pEnv->GetMsgClient())->HandleUpMsgData( macidDst.c_str(), szbuf, szlen) ;

				//_srvCaller.getTernimalByVehicleByTypeEx( DOWN_CTRL_MSG_TEXT_INFO, seqid, carnum , carcolor, inner.c_str() ) ;

				OUT_PRINT( ip, port , str_access_code.c_str(), "DOWN_CTRL_MSG_TEXT_INFO seqid %u, carnum %s, carcolor %s" , seqid , carnum, carcolor ) ;
			}
			break ;
		case DOWN_CTRL_MSG_TAKE_TRAVEL_REQ:  //2011-11-29 xfm �ϱ�������ʻ��¼����
			{
				if ( len < (int)sizeof(DownCtrlMsgTaketravelReq) ){
					OUT_ERROR( ip, port, str_access_code.c_str(), "DOWN_CTRL_MSG_TAKE_TRAVEL_REQ data length %d error" , len ) ;
					return ;
				}

				DownCtrlMsgTaketravelReq *msg = (DownCtrlMsgTaketravelReq *)data;

				unsigned int seqid = _pEnv->GetSequeue() ;

				char szKey[256]={0};
				_pEnv->GetCacheKey( seqid, szKey ) ;

				UpCtrlMsgTaketravel resp;

				resp.header.msg_len 	= ntouv32(sizeof(UpCtrlMsgTaketravel));
				resp.header.access_code = ntouv32( user._access_code ) ;
				resp.header.msg_seq 	= msg->header.msg_seq;
				resp.header.msg_type    = ntouv16( UP_CTRL_MSG ) ;
				resp.command_type       = msg->command_type;
				resp.ctrl_msg_header.data_type     = ntouv16( UP_CTRL_MSG_TAKE_TRAVEL_ACK ) ;
				resp.ctrl_msg_header.vehicle_color = msg->ctrl_msg_header.vehicle_color;
				safe_memncpy(resp.ctrl_msg_header.vehicle_no, msg->ctrl_msg_header.vehicle_no, sizeof(resp.ctrl_msg_header.vehicle_no) );

				resp.ctrl_msg_header.data_length = 0 ;

				//��ӵ����������
				_pEnv->GetMsgCache()->AddData( szKey, (const char *)&resp, sizeof(resp));
				OUT_INFO( ip, port, str_access_code.c_str() , "Add UpCtrlMsgTaketravelAck key %s", szKey );

				int szlen = 0;
				char szbuf[1024] = {0} ;
				szlen += snprintf(szbuf + szlen, 1024 - szlen, "CAITS %s %s", szKey, macidDst.c_str());
				szlen += snprintf(szbuf + szlen, 1024 - szlen, " 4 D_REQD {TYPE:4,30:%u} \r\n",  msg->command_type);

				((IMsgClient*)_pEnv->GetMsgClient())->HandleUpMsgData( macidDst.c_str(), szbuf, szlen) ;
				//_srvCaller.getTernimalByVehicleByTypeEx( DOWN_CTRL_MSG_TAKE_TRAVEL_REQ, seqid, carnum , carcolor, szbuf ) ;

				OUT_PRINT( ip, port , str_access_code.c_str(), "DOWN_CTRL_MSG_TAKE_TRAVEL_REQ seqid %u, carnum %s, carcolor %s" , seqid , carnum, carcolor ) ;
			}
			break;
		case DOWN_CTRL_MSG_EMERGENCY_MONITORING_REQ:  //2011-11-29 xfm ����������ƽ̨
		    {
		    	if ( len < (int)sizeof(DownCtrlMsgEmergencyMonitoringReq) ){
					OUT_ERROR( ip, port, str_access_code.c_str(), "DOWN_CTRL_MSG_EMERGENCY_MONITORING_REQ data length %d error" , len ) ;
					return ;
				}

				DownCtrlMsgEmergencyMonitoringReq *msg = (DownCtrlMsgEmergencyMonitoringReq *)data;

				unsigned int seqid = _pEnv->GetSequeue() ;
				char szKey[256]={0};
				_pEnv->GetCacheKey( seqid, szKey ) ;

				string inner = " 4 D_CTLM {TYPE:21,VALUE:0;" + safe2string(msg->authentication_code,sizeof(msg->authentication_code)) + ";" ;
				inner += safe2string( msg->access_point_name, sizeof(msg->access_point_name)) + ";" ;
				inner += safe2string( msg->username, sizeof(msg->username) ) + ";" ;
				inner += safe2string( msg->password, sizeof(msg->password) ) + ";" ;
				inner += safe2string( msg->server_ip , sizeof(msg->server_ip) ) + ";" ;
				inner += uitodecstr( msg->tcp_port ) + ";" ;
				inner += uitodecstr( msg->udp_port ) + ";0} \r\n" ;

				UpCtrlMsgEmergencyMonitoringAck resp;

				resp.header.msg_type    = ntouv16( UP_CTRL_MSG ) ;
				resp.header.msg_len 	= ntouv32(sizeof(UpCtrlMsgEmergencyMonitoringAck));
				resp.header.access_code = ntouv32( user._access_code ) ;
				resp.header.msg_seq 	= msg->header.msg_seq ;
				resp.ctrl_msg_header.data_type = ntouv16( UP_CTRL_MSG_EMERGENCY_MONITORING_ACK ) ;

				resp.ctrl_msg_header.vehicle_color = msg->ctrl_msg_header.vehicle_color;
				safe_memncpy(resp.ctrl_msg_header.vehicle_no, msg->ctrl_msg_header.vehicle_no, sizeof(resp.ctrl_msg_header.vehicle_no) );

				resp.ctrl_msg_header.data_length = ntouv32( sizeof(unsigned char) );
				resp.result 			= 0x00;
				//��ӵ����������
				_pEnv->GetMsgCache()->AddData( szKey, (const char *)&resp, sizeof(resp));

				int szlen = 0;
				char szbuf[1024] = {0} ;
				szlen += snprintf(szbuf + szlen, 1024 - szlen, "CAITS %s %s", szKey, macidDst.c_str());
				szlen += snprintf(szbuf + szlen, 1024 - szlen, "%s \r\n",  inner.c_str());

				((IMsgClient*)_pEnv->GetMsgClient())->HandleUpMsgData( macidDst.c_str(), szbuf, szlen);

				//_srvCaller.getTernimalByVehicleByTypeEx( DOWN_CTRL_MSG_EMERGENCY_MONITORING_REQ, seqid, carnum , carcolor, inner.c_str() ) ;

				OUT_PRINT( ip, port , str_access_code.c_str(), "DOWN_CTRL_MSG_EMERGENCY_MONITORING_REQ seqid %u, carnum %s, carcolor %s" , seqid , carnum, carcolor ) ;
		    }
			break ;
		}
	}
	else if (msg_type == DOWN_WARN_MSG ) {  // ����·������Ϣ������Ϣ
		WarnMsgHeader *warnheader = ( WarnMsgHeader *) (data + sizeof(Header)) ;
		int data_type = ntouv16( warnheader->data_type ) ;

		char carnum[128]= {0};
		safe_memncpy( carnum, warnheader->vehicle_no, sizeof(warnheader->vehicle_no) ) ;
		char carcolor[128] = {0} ;
		sprintf( carcolor, "%d" , warnheader->vehicle_color ) ;

		char macidSrc[32];
		snprintf(macidSrc, 32, "%s_%s", carcolor, carnum);

		string macidDst = "";
		if( ! _pEnv->GetRedisCache()->HGet("KCTX.PLATE2SIM", macidSrc, macidDst) || macidDst.empty()) {
			OUT_WARNING(user._ip.c_str(), user._port, user._user_name.c_str(), "KCTX.PLATE2SIM: %s not exist", macidSrc);
			return;
		}

		switch( data_type ) {
		case DOWN_WARN_MSG_URGE_TODO_REQ:  // ������������
			{
				if ( len < (int) sizeof(DownWarnMsgUrgeTodoReq) ) {
					OUT_ERROR( ip, port, str_access_code.c_str(), "DOWN_WARN_MSG_URGE_TODO_REQ data length %d error" , len ) ;
					return ;
				}

				CBase64 base64;

				DownWarnMsgUrgeTodoReq *req = ( DownWarnMsgUrgeTodoReq *) data ;

				UpWarnMsgUrgeTodoAck resp ;
				resp.header.msg_len				   = ntouv32( sizeof(UpWarnMsgUrgeTodoAck) ) ;
				resp.header.access_code 		   = ntouv32( user._access_code ) ;
				resp.header.msg_seq     		   = req->header.msg_seq ;
				resp.warn_msg_header.vehicle_color = req->warn_msg_header.vehicle_color;
				safe_memncpy(resp.warn_msg_header.vehicle_no, req->warn_msg_header.vehicle_no, sizeof(resp.warn_msg_header.vehicle_no) );

				resp.warn_msg_header.data_length   = ntouv32( sizeof(int) + sizeof(char) ) ;
				resp.supervision_id     		   = req->warn_msg_body.supervision_id ;
				resp.result						   = 0x00 ;

				unsigned int seqid = _pEnv->GetSequeue() ;
				char szKey[256]={0};
				_pEnv->GetCacheKey( seqid, szKey ) ;

				_pEnv->GetMsgCache()->AddData( szKey, (const char *)&resp, sizeof(resp) ) ;

				char supervisionEndUtc[128]={0};
				sprintf( supervisionEndUtc, "%llu", (unsigned long long)ntouv64(req->warn_msg_body.supervision_endtime) ) ;

				char supervisionId[128] = {0} ;
				sprintf( supervisionId , "%d" , ntouv32(req->warn_msg_body.supervision_id) ) ;

				char supervisionLevel[128] = {0} ;
				sprintf( supervisionLevel, "%d" , req->warn_msg_body.supervision_level ) ;

				char supervisor[256] = {0} ;
				safe_memncpy( supervisor, req->warn_msg_body.supervisor, sizeof(req->warn_msg_body.supervisor) ) ;

				char supervisorEmail[128] = {0} ;
				safe_memncpy( supervisorEmail, req->warn_msg_body.supervisor_email, sizeof(req->warn_msg_body.supervisor_email) ) ;

				char supervisorTel[128] = {0} ;
				safe_memncpy( supervisorTel, req->warn_msg_body.supervisor_tel, sizeof(req->warn_msg_body.supervisor_tel) ) ;

				char wanSrc[128] = {0} ;
				sprintf( wanSrc, "%d", req->warn_msg_body.warn_src ) ;

				char wanType[128] = {0} ;
				sprintf( wanType, "%d", ntouv16( req->warn_msg_body.warn_type ) ) ;

				char warUtc[128] = {0} ;
				sprintf( warUtc , "%llu", (unsigned long long)ntouv64(req->warn_msg_body.warn_time) ) ;

				char keyBuf[32];
				snprintf(keyBuf, 32, "%s_%s", carcolor, carnum);

				string inner = "CAITS " + string(szKey) + " " + macidDst + " 4 L_PROV {TYPE:D_WARN,WARNTODO";
				inner += ":" + string(wanSrc);
				inner += "|" + string(wanType);
				inner += "|" + string(warUtc);
				inner += "|" + string(supervisionId);
				inner += "|" + string(supervisionEndUtc);
				inner += "|" + string(supervisionLevel);
				inner += "|" + string(supervisor);
				inner += "|" + string(supervisorTel);
				inner += "|" + string(supervisorEmail);
				inner += "} \r\n";

				((IMsgClient*)_pEnv->GetMsgClient())->HandleUpMsgData( macidDst.c_str(), inner.c_str(), inner.length() );

				//_srvCaller.addMsgUrgeTodo( DOWN_WARN_MSG_URGE_TODO_REQ , seqid , supervisionEndUtc , supervisionId ,
				//		supervisionLevel , supervisor , supervisorEmail , supervisorTel , carcolor, carnum , wanSrc , wanType , warUtc ) ;

				OUT_PRINT( ip, port , str_access_code.c_str(), "DOWN_WARN_MSG_URGE_TODO_REQ seqid %u, carnum %s, carcolor %s" , seqid , carnum, carcolor ) ;
			}
			break ;
		case DOWN_WARN_MSG_INFORM_TIPS: // ����Ԥ��
		case DOWN_WARN_MSG_EXG_INFORM:  // ʵʱ����������Ϣ
			{
				if ( len < (int) sizeof(DownWarnMsgInformTips) ) {
					OUT_ERROR( ip, port, str_access_code.c_str(), "%s data length %d error" ,
							( data_type == DOWN_WARN_MSG_EXG_INFORM ) ? "DOWN_WARN_MSG_EXG_INFORM" : "DOWN_WARN_MSG_INFORM_TIPS" , len ) ;
					return ;
				}

				DownWarnMsgInformTips *req = ( DownWarnMsgInformTips *) data ;

				int nlen = ntouv32(req->warn_msg_body.warn_length) ;
				if ( len < ( int ) sizeof(req) + nlen || nlen < 0 ) {
					OUT_ERROR( ip, port, str_access_code.c_str(), "%s data length %d error" ,
							( data_type == DOWN_WARN_MSG_EXG_INFORM ) ? "DOWN_WARN_MSG_EXG_INFORM" : "DOWN_WARN_MSG_INFORM_TIPS" , len ) ;
					return ;
				}

				unsigned int seqid = _pEnv->GetSequeue() ;
				char szKey[256]={0};
				_pEnv->GetCacheKey( seqid, szKey ) ;

				CQString content ;
				content.SetString( (const char *)(data+sizeof(DownWarnMsgInformTips)) , nlen ) ;

				CBase64 base64;
				base64.Encode(content.GetBuffer(), content.GetLength());

				char alarmFrom[128] = {0} ;
				sprintf( alarmFrom, "%d", req->warn_msg_body.warn_src ) ;

				char alarmTime[128] = {0} ;
				sprintf( alarmTime, "%llu", (unsigned long long)ntouv64(req->warn_msg_body.warn_time) ) ;

				char alarmType[128] = {0} ;
				sprintf( alarmType, "%d", ntouv16(req->warn_msg_body.warn_type) ) ;

				string inner = "CAITS " + string(szKey) + " " + macidDst + " 4 L_PROV {TYPE:D_WARN,WARNTIPS";
				inner += ":" + string(alarmFrom);
				inner += "|" + string(alarmType);
				inner += "|" + string(alarmTime);
				inner += "|" + string(base64.GetBuffer());
				inner += "} \r\n";

				((IMsgClient*)_pEnv->GetMsgClient())->HandleUpMsgData( macidDst.c_str(), inner.c_str(), inner.length() );

				// ���÷��񱣴�
				//_srvCaller.addMsgInformTips( data_type , seqid , content.GetBuffer() , alarmFrom, alarmTime, alarmType , carcolor , carnum ) ;

				OUT_PRINT( ip, port , str_access_code.c_str(), "%s seqid %u, carnum %s, carcolor %s" , ( data_type == DOWN_WARN_MSG_EXG_INFORM ) ? "DOWN_WARN_MSG_EXG_INFORM" : "DOWN_WARN_MSG_INFORM_TIPS", seqid , carnum, carcolor ) ;
			}
			break ;
		}
	}
	else if ( msg_type == DOWN_EXG_MSG ) {  // ����·��̬��Ϣ������Ϣ
		ExgMsgHeader *msgheader = (ExgMsgHeader*) (data + sizeof(Header));
		int data_type = ntouv16( msgheader->data_type ) ;

		char carnum[128]= {0};
		safe_memncpy( carnum, msgheader->vehicle_no, sizeof(msgheader->vehicle_no) ) ;
		char carcolor[128] = {0} ;
		sprintf( carcolor, "%d" , msgheader->vehicle_color ) ;

		switch( data_type ) {
		case DOWN_EXG_MSG_REPORT_DRIVER_INFO: // �ϱ�������ʻԱ���ʶ����Ϣ����
			{
				if ( len < (int)sizeof(DownExgMsgReportDriverInfo) ) {
					OUT_ERROR( ip, port, str_access_code.c_str(), "DOWN_EXG_MSG_REPORT_DRIVER_INFO data length %d error" , len ) ;
					return ;
				}
				//http ��ѯ����
				DownExgMsgReportDriverInfo *msg = (DownExgMsgReportDriverInfo *)data;

				UpExgMsgReportDriverInfo resp;
				resp.header.msg_len 	= ntouv32(sizeof(UpExgMsgReportDriverInfo));
				resp.header.access_code = ntouv32( user._access_code ) ;
				resp.header.msg_seq 	= msg->header.msg_seq;
				resp.header.msg_type    = ntouv16( UP_EXG_MSG);
				resp.exg_msg_header.data_type     = ntouv16(UP_EXG_MSG_REPORT_DRIVER_INFO_ACK);
				resp.exg_msg_header.vehicle_color = msg->exg_msg_header.vehicle_color;
				safe_memncpy(resp.exg_msg_header.vehicle_no, msg->exg_msg_header.vehicle_no, sizeof(resp.exg_msg_header.vehicle_no) );
				resp.exg_msg_header.data_length = ntouv32(  sizeof(UpExgMsgReportDriverInfo) - sizeof(Header) - sizeof(ExgMsgHeader) - sizeof(Footer) );

				unsigned int seqid = _pEnv->GetSequeue() ;
				char szKey[256]={0};
				_pEnv->GetCacheKey( seqid, szKey ) ;

				// ��ӵ�������
				_pEnv->GetMsgCache()->AddData( szKey, (const char *)&resp, sizeof(resp) ) ;

				OUT_PRINT( ip, port , str_access_code.c_str(), "DOWN_EXG_MSG_REPORT_DRIVER_INFO seqid %u, carnum %s, carcolor %s" , seqid , carnum, carcolor ) ;
			}
			break ;
		case DOWN_EXG_MSG_TAKE_WAYBILL_REQ: // �ϱ����������˵�����
			{
				DownExgMsgTakeWaybillReq *msg = (DownExgMsgTakeWaybillReq *)data;

				UpExgMsgReportEwaybillInfo  resp;
				resp.header.msg_len 	= 0 ;
				resp.header.access_code = ntouv32( user._access_code ) ;
				resp.header.msg_seq 	= msg->header.msg_seq;
				resp.header.msg_type    = ntouv16( UP_EXG_MSG );
				resp.exg_msg_header.data_type     = ntouv16(UP_EXG_MSG_TAKE_WAYBILL_ACK);
				resp.exg_msg_header.vehicle_color = msg->exg_msg_header.vehicle_color;
				safe_memncpy(resp.exg_msg_header.vehicle_no, msg->exg_msg_header.vehicle_no, sizeof(resp.exg_msg_header.vehicle_no) );

				unsigned int seqid = _pEnv->GetSequeue() ;
				char szKey[256]={0};
				_pEnv->GetCacheKey( seqid, szKey ) ;

				// ��ӵ�������
				_pEnv->GetMsgCache()->AddData( szKey, (const char *)&resp, sizeof(resp) ) ;

				// ���÷��񱣴�
				//_srvCaller.getEticketByVehicle( DOWN_EXG_MSG_TAKE_WAYBILL_REQ, seqid , carnum , carcolor ) ;

				OUT_PRINT( ip, port , str_access_code.c_str(), "DOWN_EXG_MSG_TAKE_WAYBILL_REQ seqid %u, carnum %s, carcolor %s" , seqid , carnum, carcolor ) ;
			}
			break ;
		}

	}
	else if ( msg_type == DOWN_BASE_MSG ) { // ����·��̬��Ϣ������Ϣ
		BaseMsgHeader *base_header = ( BaseMsgHeader *) ( data + sizeof(Header) ) ;
		int data_type = ntouv16( base_header->data_type ) ;

		char carnum[128]= {0};
		safe_memncpy( carnum, base_header->vehicle_no, sizeof(base_header->vehicle_no) ) ;
		char carcolor[128] = {0} ;
		sprintf( carcolor, "%d" , base_header->vehicle_color ) ;

		char macidSrc[32];
		snprintf(macidSrc, 32, "%s_%s", carcolor, carnum);

		string macidDst = "";
		if( ! _pEnv->GetRedisCache()->HGet("KCTX.PLATE2SIM", macidSrc, macidDst) || macidDst.empty()) {
			OUT_WARNING(user._ip.c_str(), user._port, user._user_name.c_str(), "KCTX.PLATE2SIM: %s not exist", macidSrc);
			return;
		}

		switch( data_type ) {
		case DOWN_BASE_MSG_VEHICLE_ADDED:
			{
				if ( len < (int)sizeof(DownBaseMsgVehicleAdded) ) {
					OUT_ERROR( ip, port, str_access_code.c_str(), "DOWN_BASE_MSG_VEHICLE_ADDED data length %d error" , len ) ;
					return ;
				}

				DownBaseMsgVehicleAdded *msg = (DownBaseMsgVehicleAdded*)data ;

				// ����������̬��Ϣ
				UpbaseMsgVehicleAddedAck resp ;
				resp.header.msg_seq		= msg->header.msg_seq ;
				resp.header.access_code = ntouv32( user._access_code ) ;
				resp.msg_header.vehicle_color = base_header->vehicle_color ;
				safe_memncpy( resp.msg_header.vehicle_no, base_header->vehicle_no, sizeof(base_header->vehicle_no) ) ;

				string carinfo = "";
				if( ! _pEnv->GetRedisCache()->HGet("KCTX.CARINFO", macidSrc, carinfo) || carinfo.empty()) {
					OUT_WARNING(user._ip.c_str(), user._port, user._user_name.c_str(), "KCTX.CARINFO: %s not exist", macidSrc);
					return;
				}

				resp.header.msg_len  		= ntouv32(sizeof(UpbaseMsgVehicleAddedAck) + carinfo.length() + sizeof(Footer) ) ;
				resp.msg_header.data_length = ntouv32(carinfo.length()) ;

				DataBuffer buf;
				Footer footer;

				buf.writeBlock(&resp , sizeof(UpbaseMsgVehicleAddedAck));
				buf.writeBlock(carinfo.c_str(), carinfo.length());
				buf.writeBlock( &footer, sizeof(footer) ) ;

				HandlePasUpData(access_code, buf.getBuffer(), buf.getLength());

				OUT_PRINT( ip, port , str_access_code.c_str(), "DOWN_BASE_MSG_VEHICLE_ADDED , carnum %s, carcolor %s" , carnum, carcolor ) ;
			}
			break ;
		}
	} else {
		OUT_WARNING( ip , port , str_access_code.c_str(), "except message:%s", (const char*)data );
	}

	user._last_active_time = time(NULL) ;
	_online_user.SetUser( user._user_id, user ) ;
}

// �ر�����·����������
void PasClient::Close( int accesscode )
{
	User user = _online_user.GetUserByAccessCode( accesscode ) ;
	if ( user._user_id.empty() ) {
		OUT_ERROR( NULL, 0, NULL, "close access code %d user not exist" , accesscode ) ;
		return ;
	}
	user._user_state = User::OFF_LINE ;
	_online_user.SetUser( user._user_id, user ) ;
}

// ���µ�ǰ����·������״̬
void PasClient::UpdateSlaveConn( int accesscode, int state )
{
	User user = _online_user.GetUserByAccessCode( accesscode ) ;
	if ( user._user_id.empty() ) {
		OUT_ERROR( NULL, 0, NULL, "update slave close access code %d user not exist", accesscode ) ;
		return ;
	}
	// ��������״̬����
	//_srvCaller.updateConnectState( ( state == CONN_CONNECT ) ? DOWN_CONNECT_RSP : DOWN_DISCONNECT_RSP ,
	//		_pEnv->GetSequeue() , GetAreaCode(user), CONN_SLAVER ,  state ) ;
}

// ֱ�ӶϿ���Ӧʡ�����Ӵ���
void PasClient::Enable( int areacode , int flag )
{
	char szuser[128] = {0};
	sprintf( szuser, "%s%d" , PAS_USER_TAG , areacode ) ;

	User user = _online_user.GetUserByUserId( szuser ) ;
	if ( user._user_id.empty() ) {
		OUT_ERROR( NULL, 0, NULL, "Enable areacode user %d failed" , areacode ) ;
		return ;
	}

	// �Ƿ�������
	if ( flag & PAS_USERLINK_ONLINE ) {
		OUT_INFO(user._ip.c_str(),user._port,user._user_id.c_str(),"Send user state offline then reconnect");
		// ��������Ϊ��������
		if ( user._user_state != User::ON_LINE ) {
			user._user_state = User::OFF_LINE ;
		}
	} else if ( flag & PAS_MAINLINK_LOGOUT ){
		OUT_INFO(user._ip.c_str(),user._port,user._user_id.c_str(),"Send UpDisconnectReq, UP_DISCONNECT_REQ");
		// �Ͽ�����
		UpDisconnectReq req ;
		req.header.msg_seq 		= ntouv32(_proto_parse.get_next_seq());
		req.header.access_code  = ntouv32( user._access_code);
		req.user_id        		= ntouv32( atoi(user._user_name.c_str()) ) ;
		memcpy( req.password , user._user_pwd.c_str(), sizeof(req.password) ) ;

		SendCrcData( user._fd , (const char*)&req, sizeof(req) );

		user._user_state = User::DISABLED ;

	} else if ( flag & PAS_SUBLINK_ERROR ) {  // �������·�쳣�����
		OUT_INFO( user._ip.c_str(), user._port, user._user_id.c_str(), "PAS_SUBLINK_ERROR : UP_CLOSELINK_INFORM close double link" ) ;
		// ����·�쳣����
		_pEnv->GetPccServer()->Close( user._access_code, UP_CLOSELINK_INFORM, 0x00 ) ;
		// ģ����Ϊ�쳣ֱ�ӹر�
		if ( user._fd > 0 ) {
			CloseSocket( user._fd ) ;
			user._user_state = User::OFF_LINE ;
		}
	} else if ( flag & PAS_MAINLINK_ERROR ) { // ��������·�쳣�����
		OUT_INFO( user._ip.c_str(), user._port, user._user_id.c_str(), "PAS_MAINLINK_ERROR : UP_DISCONNECT_INFORM message" ) ;
		// ���͹رմ���·����
		_pEnv->GetPccServer()->Close( user._access_code , UP_DISCONNECT_INFORM , 0x00 ) ;
		/**
		// ģ����Ϊ�쳣ֱ�ӹر�
		if ( user._fd > 0 ) {
			CloseSocket( user._fd ) ;
			user._user_state = User::OFF_LINE ;
		}*/
	} else {  // ��������
		OUT_ERROR( user._ip.c_str(), user._port, user._user_id.c_str(), "Recv Error Enable flag %x", flag ) ;
	}
	// ����״̬Ϊ����
	_online_user.SetUser( szuser , user ) ;
}

// ���MACID��SEQID��ӳ���ϵ
void PasClient::AddMacId2SeqId( unsigned short msgid, const char *macid, const char *seqid )
{
	// ������Ӧ��������������,0x8000
	if ( ! ( msgid & 0x8000 ) ) {
		msgid |= 0x8000 ;
	}

	char key[512] = {0};
	// MAC����Ϣ�������ֶ�Ӧ���������
	sprintf( key, "%s_%d" , macid, msgid ) ;

	_macid2seqid.AddSession( key, seqid ) ;
}

// ͨ��MACID����Ϣ����ȡ�ö�Ӧ����
bool PasClient::GetMacId2SeqId( unsigned short msgid, const char *macid, char *seqid )
{
	// ������Ӧ��������������,0x8000
	if ( ! ( msgid & 0x8000 ) ) {
		msgid |= 0x8000 ;
	}

	char key[512] = {0};
	// MAC����Ϣ�������ֶ�Ӧ���������
	sprintf( key, "%s_%d" , macid, msgid ) ;

	string val ;
	if ( ! _macid2seqid.GetSession( key, val ) ){
		return false ;
	}
	sprintf( seqid, "%s" , val.c_str() ) ;

	return true ;
}

// ȡ�õ�ǰ�û����������
int PasClient::GetAreaCode( User &user )
{
	if ( user._user_id.empty() ) {
		return 0 ;
	}
	return atoi((const char *)( user._user_id.c_str() + PAS_TAG_LEN ));
}

// ���ܴ�������
bool PasClient::EncryptData( unsigned char *data, unsigned int len , bool encode )
{
	if ( len < sizeof(Header) )
		return false ;

	Header *header = ( Header *) data ;
	// �Ƿ���Ҫ���ܴ���
	if ( ! header->encrypt_flag && ! encode ) {
		return false;
	}

	int M1 = 0, IA1 = 0 , IC1 = 0 ;
	int accesscode = ntouv32( header->access_code ) ;
	// ��Կ�Ƿ�Ϊ�����Ϊ�ղ���Ҫ����
	if ( ! _pEnv->GetUserKey(accesscode, M1, IA1, IC1 ) ) {
		return false ;
	}
	// printf( "M1: %d, IA1: %d, IC1: %d\n" , M1, IA1, IC1 ) ;

	// ���Ϊ���ܴ���
	if ( encode ) {
		// ���ü��ܱ�־λ
		header->encrypt_flag =  1 ;
		// ��Ӽ�����Կ
		header->encrypt_key  =  ntouv32( CEncrypt::rand_key() ) ;
	}

	// ��������
	return CEncrypt::encrypt( M1, IA1, IC1, (unsigned char *)data, (unsigned int) len ) ;
}

//========================================= �û�����  ===============================================
// ��USERINFOת��ΪUser����
void PasClient::ConvertUser( const _UserInfo &info, User &user )
{
	user._user_id     =  info.tag + info.code ;
	user._access_code =  atoi( info.type.c_str() ) ;  // ���������
	user._ip          =  info.ip ;
	user._port        =  info.port ;
	user._user_name   =  info.user ;
	user._user_pwd    =  info.pwd  ;
	user._user_type   =  info.type ;
	user._user_state  = User::OFF_LINE ;
	user._socket_type = User::TcpConnClient ;
	user._connect_info.keep_alive = AlwaysReConn ;
	user._connect_info.timeval    = 30 ;
}

void PasClient::NotifyUser( const _UserInfo &info , int op )
{
	string key = info.tag + info.code ;
	User user  = _online_user.GetUserByUserId( key ) ;

	OUT_PRINT( info.ip.c_str(), info.port, key.c_str() , "PasClient operate %d user, username %s, password %s" ,
				op , info.user.c_str(), info.pwd.c_str() ) ;

	switch( op ){
	case USER_ADDED:
		{
			ConvertUser( info, user ) ;
			// ����µ��û�
			if ( ! _online_user.AddUser( key, user ) ) {
				if ( user._fd != NULL ) {
					OUT_INFO( user._ip.c_str(), user._port, user._user_id.c_str() ,
							"PasClient Add New user close fd %d" , user._fd->_fd ) ;
					CloseSocket( user._fd ) ;
				}
				_online_user.SetUser( key, user ) ;
			}
		}
		break ;
	case USER_DELED:
		if ( ! user._user_id.empty() ) {
			if ( user._fd != NULL ) {
				OUT_INFO( user._ip.c_str(), user._port, user._user_id.c_str() ,
						"PasClient Delete User fd %d" , user._fd->_fd ) ;
				CloseSocket( user._fd ) ;
			}
			// ɾ���û�����
			_online_user.DeleteUser( key ) ;
		}
		break ;
	case USER_CHGED:
		if ( ! user._user_id.empty() ) {
			// �޸��û�����
			ConvertUser( info, user ) ;
			if ( user._fd != NULL ) {
				OUT_INFO( user._ip.c_str(), user._port, user._user_id.c_str() ,
						"PasClient Change User close fd %d" , user._fd->_fd ) ;
				CloseSocket( user._fd ) ;
			}
			_online_user.SetUser( key, user ) ;
		}
		break ;
	}
}

