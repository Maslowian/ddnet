#include "plugin_menager.h"

#include <engine/shared/config.h>
#include <engine/shared/http.h>

#include <git2.h>

CPluginMenager::CPluginMenager()
{
}

CPluginMenager::~CPluginMenager()
{
}

void CPluginMenager::ConInstallPlugin(IConsole::IResult *pResult, void *pUserData)
{
	CPluginMenager *pPluginMenager = (CPluginMenager *)pUserData;

	const char *pName = pResult->GetString(0);

	pPluginMenager->InstallPlugin(pName);
}

void CPluginMenager::ConUninstallPlugin(IConsole::IResult *pResult, void *pUserData)
{
	CPluginMenager *pPluginMenager = (CPluginMenager *)pUserData;

	const char *pName = pResult->GetString(0);

	pPluginMenager->UninstallPlugin(pName);
}

void CPluginMenager::ConListPlugins(IConsole::IResult *pResult, void *pUserData)
{
	CPluginMenager *pPluginMenager = (CPluginMenager *)pUserData;


}

void CPluginMenager::ConEnablePlugin(IConsole::IResult *pResult, void *pUserData)
{
	CPluginMenager *pPluginMenager = (CPluginMenager *)pUserData;

	const char *pName = pResult->GetString(0);

	pPluginMenager->EnablePlugin(pName);
}

void CPluginMenager::ConDisablePlugin(IConsole::IResult *pResult, void *pUserData)
{
	CPluginMenager *pPluginMenager = (CPluginMenager *)pUserData;

	const char *pName = pResult->GetString(0);

	pPluginMenager->DisablePlugin(pName);
}

void CPluginMenager::OnInit()
{
	dbg_assert(git_libgit2_init() < 0, "Failed to initialize libgit2");
}

void CPluginMenager::OnReset()
{
}

void CPluginMenager::OnShutdown()
{
	dbg_assert(git_libgit2_shutdown() < 0, "Failed to shutdown libgit2");
}

void CPluginMenager::OnConsoleInit()
{
	Console()->Register("install_plugin", "s[url|name]", CFGFLAG_CLIENT, ConInstallPlugin, this, "Install a plugin");
	Console()->Register("add_local_plugin", "s[path]", CFGFLAG_CLIENT, ConAddLocalPlugin, this, "Install a local plugin without cloning it");
	Console()->Register("remove_local_plugin", "s[path]", CFGFLAG_CLIENT, ConRemoveLocalPlugin, this, "Remove a local plugin");
	Console()->Register("uninstall_plugin", "s[name]", CFGFLAG_CLIENT, ConUninstallPlugin, this, "Uninstall a plugin");
	Console()->Register("list_plugins", "", CFGFLAG_CLIENT, ConListPlugins, this, "List all plugins");
	Console()->Register("enable_plugin", "s[name]", CFGFLAG_CLIENT, ConEnablePlugin, this, "Enable a plugin");
	Console()->Register("disable_plugin", "s[name]", CFGFLAG_CLIENT, ConDisablePlugin, this, "Disable a plugin");
}

void CPluginMenager::InstallPlugin(const char *pName)
{
}

void CPluginMenager::UninstallPlugin(const char *pName)
{
}

void CPluginMenager::EnablePlugin(const char *pName)
{
}

void CPluginMenager::DisablePlugin(const char *pName)
{
}