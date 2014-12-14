/*************************************************************************
*    UrBackup - Client/Server backup system
*    Copyright (C) 2011-2014 Martin Raiber
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
**************************************************************************/
#include "Backup.h"
#include "server_settings.h"
#include "../Interface/Server.h"
#include "database.h"
#include "../urbackupcommon/os_functions.h"
#include "server_log.h"
#include "../stringtools.h"
#include "ClientMain.h"
#include "../urlplugin/IUrlFactory.h"
#include "server_status.h"

extern IUrlFactory *url_fak;

Backup::Backup(ClientMain* client_main, int clientid, std::wstring clientname, LogAction log_action, bool is_file_backup, bool is_incremental)
	: client_main(client_main), clientid(clientid), clientname(clientname), log_action(log_action),
	is_file_backup(is_file_backup), r_incremental(is_incremental), r_resumed(false), backup_result(false),
	log_backup(true), has_early_error(false), should_backoff(true), db(NULL)
{
	status = ServerStatus::getStatus(clientname);
}

void Backup::operator()()
{
	db = Server->getDatabase(Server->getThreadID(), URBACKUPDB_SERVER);
	server_settings.reset(new ServerSettings(db, clientid));
	backup_dao.reset(new ServerBackupDao(db));

	if(log_action!=LogAction_NoLogging)
	{
		ServerLogger::reset(clientid);
	}

	ScopedActiveThread sat;

	if(is_file_backup)
	{
		if(r_incremental)
		{
			if(r_resumed)
			{
				status.statusaction=sa_resume_incr_file;
			}
			else
			{
				status.statusaction=sa_incr_file;
			}
		}
		else
		{
			if(r_resumed)
			{
				status.statusaction=sa_resume_full_file;
			}
			else
			{
				status.statusaction=sa_full_file;
			}
		}
		status.pcdone=-1;
	}
	else
	{
		if(r_incremental)
		{
			status.statusaction=sa_incr_image;
		}
		else
		{
			status.statusaction=sa_full_image;
		}
		status.pcdone=0;
	}

	ServerStatus::setServerStatus(status);
	ServerStatus::stopBackup(clientname, false);

	createDirectoryForClient();

	int64 backup_starttime=Server->getTimeMS();

	client_main->startBackupRunning(is_file_backup);

	bool do_log = false;
	backup_result = doBackup();

	client_main->stopBackupRunning(is_file_backup);

	if(!has_early_error && log_action!=LogAction_NoLogging)
	{
		ServerLogger::Log(clientid, L"Time taken for backing up client "+clientname+L": "+widen(PrettyPrintTime(Server->getTimeMS()-backup_starttime)), LL_INFO);
		if(!backup_result)
		{
			ServerLogger::Log(clientid, "Backup failed", LL_ERROR);
		}
		else
		{
			ServerLogger::Log(clientid, "Backup succeeded", LL_INFO);
		}

		ServerCleanupThread::updateStats(false);
	}

	if( (log_backup || log_action == LogAction_AlwaysLog) && log_action!=LogAction_NoLogging)
	{
		saveClientLogdata(is_file_backup?0:1, r_incremental?1:0, backup_result && !has_early_error, r_resumed); 
	}

	status.pcdone=-1;
	status.hashqueuesize=0;
	status.prepare_hashqueuesize=0;
	status.statusaction=sa_none;
	ServerStatus::setServerStatus(status);

	server_settings.reset();
	backup_dao.reset();
	db=NULL;

	client_main->getInternalCommandPipe()->Write("WAKEUP");
}

bool Backup::createDirectoryForClient(void)
{
	std::wstring backupfolder=server_settings->getSettings()->backupfolder;
	if(!os_create_dir(os_file_prefix(backupfolder+os_file_sep()+clientname)) && !os_directory_exists(os_file_prefix(backupfolder+os_file_sep()+clientname)) )
	{
		Server->Log(L"Could not create or read directory for client \""+clientname+L"\"", LL_ERROR);
		return false;
	}
	return true;
}

void Backup::saveClientLogdata(int image, int incremental, bool r_success, bool resumed)
{
	int errors=0;
	int warnings=0;
	int infos=0;
	std::wstring logdata=ServerLogger::getLogdata(clientid, errors, warnings, infos);

	backup_dao->saveBackupLog(clientid, errors, warnings, infos, is_file_backup?0:1,
		r_incremental?1:0, r_resumed?1:0);

	backup_dao->saveBackupLogData(db->getLastInsertID(), logdata);

	sendLogdataMail(r_success, image, incremental, resumed, errors, warnings, infos, logdata);

	ServerLogger::reset(clientid);
}

void Backup::sendLogdataMail(bool r_success, int image, int incremental, bool resumed, int errors, int warnings, int infos, std::wstring &data)
{
	MailServer mail_server=ClientMain::getMailServerSettings();
	if(mail_server.servername.empty())
		return;

	if(url_fak==NULL)
		return;

	std::vector<int> mailable_user_ids = backup_dao->getMailableUserIds();
	for(size_t i=0;i<mailable_user_ids.size();++i)
	{
		std::wstring logr=getUserRights(mailable_user_ids[i], "logs");
		bool has_r=false;
		if(logr!=L"all")
		{
			std::vector<std::wstring> toks;
			Tokenize(logr, toks, L",");
			for(size_t j=0;j<toks.size();++j)
			{
				if(watoi(toks[j])==clientid)
				{
					has_r=true;
				}
			}
		}
		else
		{
			has_r=true;
		}

		if(has_r)
		{
			ServerBackupDao::SReportSettings report_settings =
				backup_dao->getUserReportSettings(mailable_user_ids[i]);

			if(report_settings.exists)
			{
				std::wstring report_mail=report_settings.report_mail;
				int report_loglevel=report_settings.report_loglevel;
				int report_sendonly=report_settings.report_sendonly;

				if( ( ( report_loglevel==0 && infos>0 )
					|| ( report_loglevel<=1 && warnings>0 )
					|| ( report_loglevel<=2 && errors>0 ) ) &&
					(report_sendonly==0 ||
					( report_sendonly==1 && !r_success ) ||
					( report_sendonly==2 && r_success)) )
				{
					std::vector<std::string> to_addrs;
					Tokenize(Server->ConvertToUTF8(report_mail), to_addrs, ",;");

					std::string subj="UrBackup: ";
					std::string msg="UrBackup just did ";
					if(incremental>0)
					{
						if(resumed)
						{
							msg+="a resumed incremental ";
							subj+="Resumed incremental ";
						}
						else
						{
							msg+="an incremental ";
							subj+="Incremental ";
						}
					}
					else
					{
						if(resumed)
						{
							msg+="a resumed full ";
							subj+="Resumed full ";
						}
						else
						{
							msg+="a full ";
							subj+="Full ";
						}
					}

					if(image>0)
					{
						msg+="image ";
						subj+="image ";
					}
					else
					{
						msg+="file ";
						subj+="file ";
					}
					subj+="backup of \""+Server->ConvertToUTF8(clientname)+"\"\n";
					msg+="backup of \""+Server->ConvertToUTF8(clientname)+"\".\n";
					msg+="\nReport:\n";
					msg+="( "+nconvert(infos);
					if(infos!=1) msg+=" infos, ";
					else msg+=" info, ";
					msg+=nconvert(warnings);
					if(warnings!=1) msg+=" warnings, ";
					else msg+=" warning, ";
					msg+=nconvert(errors);
					if(errors!=1) msg+=" errors";
					else msg+=" error";
					msg+=" )\n\n";
					std::vector<std::wstring> msgs;
					TokenizeMail(data, msgs, L"\n");

					for(size_t j=0;j<msgs.size();++j)
					{
						std::wstring ll;
						if(!msgs[j].empty()) ll=msgs[j][0];
						int li=watoi(ll);
						msgs[j].erase(0, 2);
						std::wstring tt=getuntil(L"-", msgs[j]);
						std::wstring m=getafter(L"-", msgs[j]);
						tt=backup_dao->formatUnixtime(watoi64(tt)).value;
						std::string lls="info";
						if(li==1) lls="warning";
						else if(li==2) lls="error";
						msg+=Server->ConvertToUTF8(tt)+"("+lls+"): "+Server->ConvertToUTF8(m)+"\n";
					}
					if(!r_success)
						subj+=" - failed";
					else
						subj+=" - success";

					std::string errmsg;
					bool b=url_fak->sendMail(mail_server, to_addrs, subj, msg, &errmsg);
					if(!b)
					{
						Server->Log("Sending mail failed. "+errmsg, LL_WARNING);
					}
				}
			}
		}
	}
}

std::wstring Backup::getUserRights(int userid, std::string domain)
{
	if(domain!="all")
	{
		if(getUserRights(userid, "all")==L"all")
			return L"all";
	}

	ServerBackupDao::CondString t_right = backup_dao->getUserRight(userid, widen(domain));
	if(t_right.exists)
	{
		return t_right.value;
	}
	else
	{
		return L"none";
	}
}
