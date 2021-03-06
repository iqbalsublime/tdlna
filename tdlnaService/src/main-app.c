/*
 * Copyright 2014 Samsung Electronics Co., Ltd All Rights Reserved
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "main-app.h"
#include "defines-app.h"
#include "types-app.h"
#include "logger.h"
#include "proxy-client.h"
#include "tdlnamain.h"
#include "metadata.h"

#include <service_app.h>
#include <stdlib.h>
#include <stdio.h>
#include <dlog.h>

#include <system_settings.h>
#include <metadata_extractor.h>
#include <media_content.h>

//T-DLNA Service Application part

// app event callbacks
static bool _on_create_cb(void *user_data);
static void _on_terminate_cb(void *user_data);
static void _on_app_control_callback(app_control_h app_control, void *user_data);

static int _app_init(app_data *app);
static int _app_send_response(app_data *app, bundle *const msg);
static int _app_execute_operation(app_data *appdata, req_operation operation_type);
static int _app_process_received_message(bundle *rec_msg, bundle *resp_msg, req_operation *req_oper);
static int _on_proxy_client_msg_received_cb(void *data, bundle *const rec_msg);
//static void _media_search_completed_cb(media_content_error_e error,void* user_data);
static void get_DeviceID();
static int sendResponMessage(void *data);

char deviceName[64], shared_folder[512];
char sharing_folders[FOLDER_COUNT][512];
int folder_length=0;
char send_folders[513000] = {'\0',};

app_data *app_create()
{
    app_data *app = calloc(1, sizeof(app_data));
    return app;
}

void app_destroy(app_data *app)
{
    free(app);
    dlog_print(DLOG_INFO ,"tdlna", "app_destroy TDLNA앱이 종료됨!");
}

int app_run(app_data *app, int argc, char **argv)
{
    RETVM_IF(!app, -1, "Application data is NULL");

    service_app_lifecycle_callback_s cbs =
    {
        .create = _on_create_cb,
        .terminate = _on_terminate_cb,
        .app_control = _on_app_control_callback
    };

    return service_app_main(argc, argv, &cbs, app);
}

static bool _on_create_cb(void *user_data)
{
	dlog_print(DLOG_INFO ,"tdlna", "_on_create_cb 실행");
    RETVM_IF(!user_data, false, "User_data is NULL");
    RETVM_IF(_app_init(user_data) != SVC_RES_OK, false, "Failed to initialise application data");
    return true;
}

static void _on_terminate_cb(void *user_data)
{
	dlog_print(DLOG_INFO ,"tdlna", "_on_terminate_cb 실행");
    if (user_data)
    {
        app_data *app = user_data;

        proxy_client_destroy(app->proxy_client);
    }
}

void _on_app_control_callback(app_control_h app_control, void *user_data)
{
	dlog_print(DLOG_INFO ,"tdlna", "_on_app_control_callback 실행");
}

static int _on_proxy_client_msg_received_cb(void *data, bundle *const rec_msg)
{
	dlog_print(DLOG_INFO ,"tdlna", "_on_proxy_client_msg_received_cb 실행");
    int result = SVC_RES_FAIL;
    RETVM_IF(!data, result, "Data is NULL");

    app_data *app = data;
    req_operation req_operation = REQ_OPER_NONE;

    bundle *resp_msg = bundle_create();
    RETVM_IF(!resp_msg, result, "Failed to create bundle");

    result = _app_process_received_message(rec_msg, resp_msg, &req_operation);
    if (result != SVC_RES_OK)
    {

        ERR("Failed to generate response bundle");
        bundle_free(resp_msg);
        return result;
    }

    result = _app_execute_operation(app, req_operation);
    if(result == SVC_RES_OK)
    {
        result = _app_send_response(app, resp_msg);
        if (result != SVC_RES_OK)
        {
            ERR("Failed to send message to remote application");
        }
    }
    else
    {
        ERR("Failed to execute operation");
    }
    bundle_free(resp_msg);

    return result;
}

//앱 구동에 필요한 각종 appdata를 초기화
static int _app_init(app_data *app)
{
	dlog_print(DLOG_INFO ,"tdlna", "_app_init 실행");
    int result = SVC_RES_FAIL;
    RETVM_IF(!app, result, "Application data is NULL");

    app->proxy_client = proxy_client_create();
    RETVM_IF(!app->proxy_client, result, "Failed to create proxy client");

    result = proxy_client_register_port(app->proxy_client, LOCAL_MESSAGE_PORT_NAME);
    if (result != SVC_RES_OK)
    {
        ERR("Failed to register proxy client port");
        proxy_client_destroy(app->proxy_client);
        app->proxy_client = NULL;
        return result;
    }

    result = proxy_client_register_msg_receive_callback(app->proxy_client, _on_proxy_client_msg_received_cb, app);
    if (result != SVC_RES_OK)
    {
        ERR("Failed to register proxy client on message receive callback");
        proxy_client_destroy(app->proxy_client);
        app->proxy_client = NULL;
        return result;
    }


    result = pthread_mutex_init(&app->proxy_locker, NULL);
    if(result != 0)
    {
        ERR("Failed to init message response mutex ");
        proxy_client_destroy(app->proxy_client);
        app->proxy_client = NULL;
        return SVC_RES_FAIL;
    }

    //tldna 서비스 appdata초기화
    app->run_tdlna = false;
    app->tdlna_td = 0;
    get_DeviceID();//기본 기기 아이디값 가져오기
    strcpy(app->deviceName,deviceName);
    return SVC_RES_OK;
}

static int _app_process_received_message(bundle *rec_msg,
        bundle *resp_msg,
        req_operation *req_oper)
{
	dlog_print(DLOG_INFO ,"tdlna", "_app_process_received_message 실행");
    RETVM_IF(!rec_msg, SVC_RES_FAIL,"Received message is NULL");
    RETVM_IF(!resp_msg, SVC_RES_FAIL,"Response message is NULL");

    const char *resp_key_val = NULL;
    char *rec_key_val = NULL,*rec_share_folder = NULL,*rec_unshare_folder = NULL;
    int res_shared = 0;
    bool reciveOK = false;

	res_shared = bundle_get_str(rec_msg, "shared", &rec_share_folder);
    if (res_shared == BUNDLE_ERROR_NONE) {//공유 폴더 수신
    	reciveOK = true;
    	RETVM_IF(res_shared != BUNDLE_ERROR_NONE, SVC_RES_FAIL, "Failed to get string from shared_bundle");
		dlog_print(DLOG_INFO ,"tdlna", "공유폴더 수신: %s",rec_share_folder);
		resp_key_val = "(공유폴더) 수신";
		*req_oper = REQ_SHARED_FOLDER;
		strcpy(shared_folder,rec_share_folder+7);
//		strcat(shared_folder,"\%");
		dlog_print(DLOG_INFO ,"tdlna", "공유폴더 저장: %s",shared_folder);
	}


	res_shared = bundle_get_str(rec_msg, "unshared", &rec_unshare_folder);
	if (res_shared == BUNDLE_ERROR_NONE) {//공유 취소 폴더수신
		reciveOK = true;
		RETVM_IF(res_shared != BUNDLE_ERROR_NONE, SVC_RES_FAIL, "Failed to get string from unshared_bundle");
		dlog_print(DLOG_INFO ,"tdlna", "공유취소폴더 수신: %s",rec_unshare_folder);
		resp_key_val = "(공유취소폴더) 수신";
		*req_oper = REQ_UNSHARED_FOLDER;
		strcpy(shared_folder,rec_unshare_folder+7);
//		strcat(shared_folder,"\%");
		dlog_print(DLOG_INFO ,"tdlna", "공유취소폴더 저장: %s",rec_unshare_folder);
	}

    int res = bundle_get_str(rec_msg, "command", &rec_key_val);
    if (res == BUNDLE_ERROR_NONE){
    	reciveOK = true;
		RETVM_IF(res != BUNDLE_ERROR_NONE, SVC_RES_FAIL, "Failed to get string from bundle");
		dlog_print(DLOG_INFO,"tdlna","웹앱으로 부터 서비스 수신:%s",rec_key_val);

		if (strcmp(rec_key_val, "server state") == 0) {//현재 상태 확인 요청
			dlog_print(DLOG_INFO ,"tdlna", "서비스 상태확인요청");
			resp_key_val = "(state) 수신";
			*req_oper = REQ_OPER_STATE;
		}
		else if(strcmp(rec_key_val,"media folder") == 0){
			dlog_print(DLOG_INFO ,"tdlna", "서비스 상태확인요청");
			resp_key_val = "(media folder) 수신";
			*req_oper = REQ_OPER_FOLDER;
		}
		else if (strcmp(rec_key_val,"meta") == 0)
		{
			resp_key_val = "metaget";
			*req_oper = REQ_OPER_META_GET_APP;
		}
		else if (strcmp(rec_key_val,"dlna on") == 0)//서비스 ON 요청
		{
			dlog_print(DLOG_INFO ,"tdlna", "서비스 ON 요청 app_process_received_message");
			resp_key_val = "(dlna 실행)수신..";
			*req_oper = REQ_OPER_DLNA_APP;
			//*req_oper = REQ_OPER_EXIT_APP;
		}
		else if (strcmp(rec_key_val,"dlna off") == 0)//서비스 ON 요청
		{
			dlog_print(DLOG_INFO ,"tdlna", "서비스 OFF 요청 app_process_received_message");
			resp_key_val = "(dlna 종료)수신..";
			*req_oper = REQ_OPER_DLNA_APP_OFF;
			//*req_oper = REQ_OPER_EXIT_APP;
		}
		else if (strstr(rec_key_val, "getDeviceId") != NULL) {
			dlog_print(DLOG_INFO, "tdlna","디바이스ID 요청 app_process_received_message");
			char *str = strtok(rec_key_val, "|");
			dlog_print(DLOG_INFO, "tdlna","strtok: %s",str);
			str = strtok(NULL, "|");
			dlog_print(DLOG_INFO, "tdlna","strtok: %s",str);

			if(str != NULL){
				if(strcmp(str,"&") == 0){//사용자가 빈값을 입력했을때, 장치 기본값으로 변경
					get_DeviceID();
				}else{//새로운 name을 저장
					strcpy(deviceName,str);
				}
			}
			resp_key_val = "(getDeviceId)수신";
			*req_oper = REQ_OPER_DEVICE_ID;
		}
    }
  	if(!reciveOK)
	{
    	dlog_print(DLOG_ERROR ,"tdlna", "처리를 못하는 명령");
		resp_key_val = "unsupported";
		*req_oper = REQ_OPER_NONE;
	}

    RETVM_IF(bundle_add_str(resp_msg, "server", resp_key_val) != 0, SVC_RES_FAIL, "Failed to add data by key to bundle");

    return SVC_RES_OK;
}
void insertSharingList(){//공유 폴더 리스트 추가
	//중복 파일 확인
	//폴더 추가
	strcpy(sharing_folders[folder_length],shared_folder);
	folder_length++;
	if(folder_length > FOLDER_COUNT ){//공유 폴더 갯수 초과시 오류
		dlog_print(DLOG_ERROR,"tdlna","sharing folder count error!");
		folder_length--;
		return;
	}
	dlog_print(DLOG_INFO,"tdlna","inserted SharingFolder[%d] : %s",folder_length-1,sharing_folders[folder_length-1]);
}
void deleteSharingList(){//공유 폴더 리스트 삭제
	//탐색
	int index=0;
	bool delete_ok=false;
	for(index = 0 ; index < folder_length ; index++){
		if(strcmp(sharing_folders[index],shared_folder) == 0 ){
			delete_ok = true;
			//폴더 삭제
			dlog_print(DLOG_INFO,"tdlna","finded folder[%d] : %s",index,sharing_folders[index]);
			int update_index = 0;
			for(update_index = index; update_index < folder_length - 1; update_index++ ){
				//지울 인덱스 부터 값이 저장되어있는 마지막 인덱스 앞 까지
				//하나씩 위로 땡긴다
				strcpy(sharing_folders[update_index],sharing_folders[update_index+1]);
			}
			strcpy(sharing_folders[folder_length-1],"\0");//마지막것은 삭제
			folder_length--;
		}
	}
	if(!delete_ok){//삭제 실패
		dlog_print(DLOG_ERROR,"tdlna","Don't find delete folder : %s",shared_folder);
		return;
	}
}
bool stateSharingList(char* f_path){//공유 폴더 리스트 삭제
	//탐색
	dlog_print(DLOG_INFO,"tdlna","공유 폴더 인지 조회! %s",f_path);
	int index=0;
	for(index = 0 ; index < folder_length ; index++){
		if(strcmp(sharing_folders[index],f_path) == 0 ){
			dlog_print(DLOG_INFO,"tdlna","finded folder[%d] : %s",index,sharing_folders[index]);
			return true;//공유중임
		}
	}
	return false;//공유하고 있지 않음
}
static int _app_execute_operation(app_data *appdata, req_operation operation_type)
{
	dlog_print(DLOG_INFO ,"tdlna", "_app_execute_operation 실행");
	bundle *resp_msg = bundle_create();

    RETVM_IF(!appdata, SVC_RES_FAIL, "Application data is NULL");

    char *resp_key_val = NULL;

	char respStr[50];
    switch (operation_type)
    {
		case REQ_OPER_STATE:
			dlog_print(DLOG_INFO, "tdlna", "현재 상태 얻기");
			if ((appdata->run_tdlna) == 0) {
				// 서비스가 꺼져있는 상태라면
				dlog_print(DLOG_INFO, "tdlna", "서비스 상태 조회 %d", appdata->run_tdlna);
				resp_key_val = "STATE:OFF";
			} else {
				resp_key_val = "STATE:ON";
				dlog_print(DLOG_INFO, "tdlna", "서비스 상태 조회 %d", appdata->run_tdlna);
			}
			break;

		case REQ_OPER_FOLDER:
			dlog_print(DLOG_INFO, "tdlna", "미디어 정보 얻기");
			send_folders[0] = '\0';//초기화
			if(media_Directory(appdata)){//미디어 폴더 경로를 sendFolder함수로 전달해줌
				//폴더검색후
				sendResponMessage(appdata);
	        	dlog_print(DLOG_INFO,"tdlna","미디어 폴더 전송:%s",send_folders);
			}
			resp_key_val = "미디어 폴더 요청";

		break;

        case REQ_OPER_META_GET_APP:
        	dlog_print(DLOG_INFO,"tdlna","메타정보 가져오기 실행 ");
//------------------------------------------------------------------------------------------------------김태형~~!!!
        	char* testDir;
        	mediaDirectory_folder(&testDir,2);
        	dlog_print(DLOG_INFO,"tdlna","비디오 폴더 이어붙인것:%s",testDir);
//-------------------------------------------------------------------------------------------------------

 //       	_media_search(appdata);

//        	Meta_Get_from_path(appdata,"/opt/usr/media/DCIM/Camera/%");


//        	int videoC = 0,imageC=0,musicC = 0 ;
//        	media_Count(&videoC,&imageC,&musicC,"/opt/usr/media/DCIM/Camera/%");

        	break;
        case REQ_OPER_DLNA_APP://실행 요청시
        	dlog_print(DLOG_INFO,"tdlna","dlna on 처리");

        	if(!(appdata->run_tdlna)){
        		// 서비스가 꺼져있는 상태라면
        		if(appdata->tdlna_td != 0){
        			dlog_print(DLOG_ERROR,"tdlna", "이전 실행된 서비스가 정상적으로 종료되지 않았습니다.");
        			return 0;
        		}
        		if(serviceOn(appdata)){
        			dlog_print(DLOG_INFO,"tdlna","★ 서비스 ON ★ %d", appdata->run_tdlna);
        			resp_key_val = "DLNA:ON";
        		}else{
					dlog_print(DLOG_INFO,"tdlna","★ 실행 실패! ★ %d", appdata->run_tdlna);
					resp_key_val = "DLNA:Failed";
        		}
        	}
        	else{
        		resp_key_val = "DLNA:RUNNING";
        		dlog_print(DLOG_INFO,"tdlna","★ 이미 실행중 ★ %d", appdata->run_tdlna);
        	}
        	break;

        case REQ_OPER_DLNA_APP_OFF://종료 요청시
			if (!(appdata->run_tdlna)) {// 서비스가 꺼져있는 상태라면
				resp_key_val = "DLNA:OFF";
				dlog_print(DLOG_INFO, "tdlna", "★ 이미 종료상태★ %d",appdata->run_tdlna);
			} else {
				serviceOff(appdata);
				resp_key_val = "DLNA:OFF";
				dlog_print(DLOG_INFO, "tdlna", "★ 서비스 OFF ★ %d",appdata->run_tdlna);
			}
			break;
        case REQ_OPER_DEVICE_ID://tDlnaName 주기
			if(deviceName){
				strcpy(appdata->deviceName, deviceName);
				setDeviceProperty(appdata);//tdlnamain으로 전달
				sprintf(respStr, "%s%s", "tDlnaName/", deviceName);
			}else
				sprintf(respStr, "%s%s", "tDlnaName/", "nameError!");
			resp_key_val = respStr;
			dlog_print(DLOG_INFO, "tdlna", "resp_key_val값 가져오기 %s",resp_key_val);
			break;
        case REQ_SHARED_FOLDER:
			resp_key_val = "공유폴더!";
			dlog_print(DLOG_INFO, "tdlna", "%s 폴더 공유 실행",shared_folder);
			insertSharingList();
//			_META *test;
//			int testC=0;
//			testC= Meta_Get_from_path(appdata,shared_folder,2,&test);
//			dlog_print(DLOG_INFO, "tdlna", "리스트갯수:%d",testC);
//			dlog_print(DLOG_INFO, "tdlna", "리스트 1 :%s",test[1].path);
//			free(test);
        	break;
        case REQ_UNSHARED_FOLDER:
         	resp_key_val = "공유해제 폴더!";
			dlog_print(DLOG_INFO, "tdlna", "%s 폴더 공유 해제 실행",shared_folder);
			deleteSharingList();
         	//공유 해제 폴더 처리 @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
			break;
        default:
            DBG("Unknown request id");
            return SVC_RES_FAIL;
            break;
    }
    RETVM_IF(bundle_add_str(resp_msg, "server", resp_key_val) != 0, SVC_RES_FAIL, "Failed to add data by key to bundle");
    _app_send_response(appdata, resp_msg);
    bundle_free(resp_msg);
    return SVC_RES_OK;
}

static int sendResponMessage(void *data){
	app_data *appdata = data;
	bundle *resp_dir = bundle_create();

	RETVM_IF(bundle_add_str(resp_dir, "folder_path", send_folders) != 0,
			SVC_RES_FAIL, "Failed to add data by key to bundle");
	_app_send_response(appdata, resp_dir);
	bundle_free(resp_dir);
	return SVC_RES_OK;
}

int sendFolder(void *data, char* dir){
	//미디어 폴더(dir)를 웹앱으로 전달
//	app_data *appdata = data;
	char sendingDirectory[512];
//	int videoCount=0,musicCount=0,imageCount=0;
	//현재 공유중인 폴더인지 조회
	if(stateSharingList(dir)){//공유 상태임
		sprintf(sendingDirectory, "%s%s", "*folder:", dir);
	}else{
		sprintf(sendingDirectory, "%s%s", "folder:", dir);
	}
	//공유폴더 조회 END
	strcat(send_folders,sendingDirectory);
	strcat(send_folders,"|");
	dlog_print(DLOG_INFO, "tdlna", "send_folders(폴더):%s", send_folders);


//	bundle *resp_dir = bundle_create();
//
//	RETVM_IF(bundle_add_str(resp_dir, "folder_path", sendingDirectory) != 0,
//			SVC_RES_FAIL, "Failed to add data by key to bundle");
//	_app_send_response(appdata, resp_dir);
//	bundle_free(resp_dir);
	//폴더 전송 END

	//폴더내 컨텐츠 갯수 전달
//	sprintf(sendingDirectory, "%s%s", dir, "/%");
//	dlog_print(DLOG_INFO, "tdlna", "컨텐츠용 sendFolder:%s", sendingDirectory);
//	media_Count(&videoCount,&imageCount,&musicCount,sendingDirectory);
//	sprintf(sendingDirectory, "%d%c%d%c%d", videoCount,'|',imageCount,'|',musicCount);
//	dlog_print(DLOG_INFO, "tdlna", "sendContents(폴더):%s", sendingDirectory);
//	resp_dir = bundle_create();
//	RETVM_IF(bundle_add_str(resp_dir, "folder_contents", sendingDirectory) != 0,
//			SVC_RES_FAIL, "Failed to add data by key to bundle");
//	_app_send_response(appdata, resp_dir);
//	bundle_free(resp_dir);
	//컨텐츠 갯수 전달 END
	return SVC_RES_OK;
}

static void get_DeviceID()
{
   char* string_ret[5];
   int ret;
   ret = system_settings_get_value_string((system_settings_key_e)SYSTEM_SETTINGS_KEY_DEVICE_NAME, &(string_ret[0]));
   dlog_print(DLOG_INFO, "tdlna","%d", ret);
   dlog_print(DLOG_INFO, "tdlna","%s", string_ret[0]);
   strcpy(deviceName, string_ret[0]);//디바이스 ID
}


static int _app_send_response(app_data *app, bundle *const msg)
{
	dlog_print(DLOG_INFO ,"tdlna", "_app_send_response 실행");
    int res = SVC_RES_FAIL;

    pthread_mutex_lock(&app->proxy_locker);
    res = proxy_client_send_message(app->proxy_client, msg);
    pthread_mutex_unlock(&app->proxy_locker);

    return res;
}
