#ifndef GAME_CLIENT_COMPONENTS_PLUGIN_MENAGER_H
#define GAME_CLIENT_COMPONENTS_PLUGIN_MENAGER_H

#include <game/client/component.h>
#include <engine/console.h>

#include <git2.h>

class CPluginMenager : public CComponent
{
public:
	enum class EPremission
	{
		NONE = 0,

		READ = 1 << 0,
		WRITE = 1 << 1,

		READ_WRITE = READ | WRITE,

		DDNET_ORG_CURL = 1 << 2,
		CURL = 1 << 3 | DDNET_ORG_CURL,

		FOLDER_ASSETS = 1 << 4,
		FOLDER_ROOT = 1 << 5,
		FOLDER_HOME = 1 << 6,

		ALL = READ_WRITE | CURL,
		DEFAULT = READ | FOLDER_HOME | DDNET_ORG_CURL,
	};

	enum
	{
		PLUGIN_MAX_URL_LENGTH = 256,

		PLUGIN_MAX_NAME_LENGTH = 128,

		PLUGIN_STATE_UNKNOWN = 0,
		PLUGIN_STATE_MISSING = 1,
		PLUGIN_STATE_INSTALLED = 2,
		PLUGIN_STATE_INSTALLING = 3,
		PLUGIN_STATE_UNINSTALLING = 4,
		PLUGIN_STATE_UPDATING = 5,
	};

	struct CPluginInfo
	{
		std::vector<char[0]> m_aDependecies;

		EPremission m_Premissions;

		char m_aInitFile[IO_MAX_PATH_LENGTH];
	};

	struct CPluginInstaller
	{
		char m_aUrl[PLUGIN_MAX_URL_LENGTH];
		char m_aPath[IO_MAX_PATH_LENGTH];

		git_repository *m_pRepo = nullptr;

		git_clone_options m_CloneOptions = GIT_CLONE_OPTIONS_INIT;

		std::atomic<float> m_Progress = 0.0f;
	};

	struct CPlugin
	{
		CPlugin(const char aValue[512]);

		void UpdateState();

		void Install();
		void Uninstall();

		void InitPlugin();

		union
		{
			CPluginInfo m_Info;
			CPluginInstaller m_Installer;
		};
		int m_State = PLUGIN_STATE_UNKNOWN;


		char m_aName[PLUGIN_MAX_NAME_LENGTH];
		union
		{
			char m_aUrl[PLUGIN_MAX_URL_LENGTH];
			char m_aLocalPath[IO_MAX_PATH_LENGTH];
		};
		int m_TagType = -1;

		bool m_Enabled = false;
	};


public:
	CPluginMenager();
	~CPluginMenager() override;

	int Sizeof() const override { return sizeof(*this); }

	static void ConInstallPlugin(IConsole::IResult *pResult, void *pUserData);
	static void ConUninstallPlugin(IConsole::IResult *pResult, void *pUserData);
	static void ConListPlugins(IConsole::IResult *pResult, void *pUserData);
	static void ConEnablePlugin(IConsole::IResult *pResult, void *pUserData);
	static void ConDisablePlugin(IConsole::IResult *pResult, void *pUserData);

	void OnInit() override;
	void OnReset() override;
	void OnShutdown() override;

	void OnConsoleInit() override;

	void InstallPlugin(const char *pName);
	void UninstallPlugin(const char *pName);

	void EnablePlugin(const char *pName);
	void DisablePlugin(const char *pName);

private:
	std::list<CPlugin> m_Plugins;
};

#endif