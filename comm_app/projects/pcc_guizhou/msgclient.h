/**********************************************
 * msgclient.h
 *
 *  Created on: 2011-07-28
 *    Author:   humingqing
 *    Comments: ʵ������Ϣ��������ͨ���Լ�����ת��
 *********************************************/

#ifndef __MsgCLIENT_H__
#define __MsgCLIENT_H__

#include "interface.h"
#include <BaseClient.h>
#include <OnlineUser.h>
#include <time.h>
#include <Session.h>
#include <interpacker.h>
#include "statinfo.h"

#define MSG_USER_TAG  "MSGCLIENT"

class PConvert ;
class MsgClient : public BaseClient , public IMsgClient, public IUserNotify
{
public:
	MsgClient() ;
	virtual ~MsgClient() ;

	// ��ʼ��
	virtual bool Init( ISystemEnv *pEnv );
	// ��ʼ����
	virtual bool Start( void );
	// ֹͣ����
	virtual void Stop();
	// ��MSG�ϴ���Ϣ
	virtual void HandleUpMsgData( const char *code, const char *data, int len ) ;
	//���³������ģ�opΪ0��ɾ����Ϊ1������
	virtual void updateSub(const list<string> &macid, int op);
public:
	// ���ݵ���ʱ����
	virtual void on_data_arrived( socket_t *sock, const void* data, int len);
	// �Ͽ����Ӵ���
	virtual void on_dis_connection( socket_t *sock );
	// Ϊ�����ʹ��
	virtual void on_new_connection( socket_t *sock, const char* ip, int port){};

	virtual void TimeWork();
	virtual void NoopWork();
	// ������½����
	virtual int build_login_msg( User &user, char *buf,int buf_len );
	// ֪ͨ�û�״̬�仯
	virtual void NotifyUser( const _UserInfo &info , int op ) ;

protected:
	// ���͵�ָ����������û�
	bool SendDataToUser( const string &area_code, const char *data, int len ) ;
	// �׷��ڲ�����
	void HandleInnerData( socket_t *sock, const char *data, int len ) ;
	// �׷���½����
	void HandleSession( socket_t *sock, const char *data, int len ) ;
	// ���������û�
	void HandleOfflineUsers( void );
	// ���������û�
	void HandleOnlineUsers(int timeval);
	// ��USERINFOת��ΪUser����
	void ConvertUser( const _UserInfo &info, User &user ) ;
	// ���ص�ǰ�û��Ķ��ĵ�����
	void LoadSubscribe( User &user ) ;

private:
	// ����ָ��
	ISystemEnv  	 *_pEnv ;
	// ���һ�η���ʱ��
	time_t		 	  _last_handle_user_time ;
	// �����û�����
	OnlineUser   	  _online_user;
	// Э��ת������
	PConvert	     *_convert ;
	// �Ự����MAC���ӦMSG�Ĺ�ϵ
	CSessionMgr  	  _session;
	// �������
	CInterSpliter     _packspliter ;
	// ����MSG��ͳ�ƴ���
	CStatInfo		  _statinfo ;
};

#endif /* LISTCLIENT_H_ */
