//------------------------------------------------------------------------------
// File: FileConfigEngine.hh
// Author: Andreas-Joachim Peters - CERN
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2011 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your option) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

#ifndef __EOSMGM_FILECONFIGENGINE__HH__
#define __EOSMGM_FILECONFIGENGINE__HH__

#include "mgm/IConfigEngine.hh"
#include "common/DbMap.hh"
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

EOSMGMNAMESPACE_BEGIN

//------------------------------------------------------------------------------
//! Class FileCfgEngineChangeLog
//------------------------------------------------------------------------------
class FileCfgEngineChangelog : public ICfgEngineChangelog
{
public:
  //----------------------------------------------------------------------------
  //! Constructor
  //----------------------------------------------------------------------------
  FileCfgEngineChangelog() {};

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  virtual ~FileCfgEngineChangelog() {};

  //----------------------------------------------------------------------------
  //! Initialization
  //!
  //! @param chlog_file path to changelog file
  //----------------------------------------------------------------------------
  void Init(const char* chlog_file);

  //----------------------------------------------------------------------------
  //! Add entry to the changelog
  //!
  //! @param info entry info
  //!
  //! @return true if successful, otherwise false
  //----------------------------------------------------------------------------
  bool AddEntry(const char* info);

  //----------------------------------------------------------------------------
  //! Get tail of the changelog
  //!
  //! @param nlines number of lines to return
  //! @param tail string to hold the response
  //!
  //! @return true if successful, otherwise false
  //----------------------------------------------------------------------------
  bool Tail(unsigned int nlines, XrdOucString& tail);

private:
  XrdSysMutex mDbMapMutex; ///< Mutex protecting the acces to the map
  eos::common::DbMap mMap; ///< Map saving recent changes
  std::string mChLogFile; ///< Path to changelog file
};


//------------------------------------------------------------------------------
//! Class FileConfigEngine
//------------------------------------------------------------------------------
class FileConfigEngine : public IConfigEngine
{
public:
  //----------------------------------------------------------------------------
  //! Comparison function for sorted listing
  //----------------------------------------------------------------------------
  static int
  CompareCtime(const void* a, const void* b)
  {
    struct filestat {
      struct stat buf;
      char filename[1024];
    };
    return ((((struct filestat*) a)->buf.st_mtime) - ((struct filestat*)
            b)->buf.st_mtime);
  }

  //----------------------------------------------------------------------------
  //! Constructor
  //!
  //! @param config_dir Directory holding the configuration files
  //----------------------------------------------------------------------------
  FileConfigEngine(const char* config_dir);

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  virtual ~FileConfigEngine();

  //----------------------------------------------------------------------------
  //! Load a given configuratino file
  //!
  //! @param env environment holding info about the configuration to be loaded
  //! @param err string holding any errors
  //!
  //! @return true if loaded successfully, otherwise false
  //----------------------------------------------------------------------------
  bool LoadConfig(XrdOucEnv& env, XrdOucString& err);

  //----------------------------------------------------------------------------
  //! Save configuration to specified destination
  //!
  //! @param env environment holding info about the destination where the
  //!        current configuration will be saved
  //! @param err string holding any errors
  //!
  //! @return true if saved successfully, otherwise false
  //----------------------------------------------------------------------------
  bool SaveConfig(XrdOucEnv& env, XrdOucString& err);

  //----------------------------------------------------------------------------
  //! List all configurations
  //!
  //! @param configlist string holding the list of all configurations
  //! @param showbackup if true then show also the backups
  //!
  //! @return true if listing successfull, otherwise false
  //----------------------------------------------------------------------------
  bool ListConfigs(XrdOucString& configlist, bool showbackups = false);

  //----------------------------------------------------------------------------
  //! Get configuration changes
  //!
  //! @param diffs string holding the configuration changes
  //----------------------------------------------------------------------------
  void Diffs(XrdOucString& diffs);

  //----------------------------------------------------------------------------
  //! Do an autosave
  //----------------------------------------------------------------------------
  bool AutoSave();

  //----------------------------------------------------------------------------
  //! Set a configuration value
  //!
  //! @param prefix identifies the type of configuration parameter
  //! @param key key of the configuration to set
  //! @param val value of the configuration
  //! @param tochangelog if true add entry also to the changelog
  //----------------------------------------------------------------------------
  void SetConfigValue(const char* prefix, const char* key,
                      const char* val, bool tochangelog = true);

  //----------------------------------------------------------------------------
  //! Delete a configuration value
  //!
  //! @param prefix identifies the type of configuration parameter
  //! @param key key of the configuration to delete
  //! @param tochangelog if true add entry also to the changelog
  //----------------------------------------------------------------------------
  void DeleteConfigValue(const char* prefix, const char* key,
                         bool tochangelog = true);

  //----------------------------------------------------------------------------
  //! Set configuration directory
  //!
  //! @param configdir configuration directory
  //----------------------------------------------------------------------------
  void SetConfigDir(const char* configdir);

  //----------------------------------------------------------------------------
  //! Push a configuration to Redis (not invoked in case of FileConfig)
  //----------------------------------------------------------------------------
  bool PushToRedis(XrdOucEnv& env, XrdOucString& err)
  {
    return true;
  }

private:
  //----------------------------------------------------------------------------
  //! Filter configuration and store in output string
  //!
  //! @param info
  //! @param out output string
  //! @param cfg_fn configuration file to be loaded
  //----------------------------------------------------------------------------
  void FilterConfig(PrintInfo& info, XrdOucString& out, const char* cfg_fn);
};

EOSMGMNAMESPACE_END

#endif
