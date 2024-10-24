#ifndef GAME_CLIENT_COMPONENTS_LUA_H
#define GAME_CLIENT_COMPONENTS_LUA_H

#include <game/client/ui.h>
#include <engine/console.h>
#include <game/client/component.h>
#include <game/client/ui_rect.h>
#include <engine/shared/config.h>

#include <optional>
#include <unordered_map>
#include <variant>

struct lua_State;

class CLua : public CComponent
{
private:
	class CRenderLayer : public CComponent
	{
	public:
		int Sizeof() const override { return sizeof(*this); }

		int Index() const;

		void OnRender() override;

	private:
		int : sizeof(CComponent) ? 0 : 8;
	};

public:
	enum
	{
		LUA_RENDER_LAYER_FIRST,
		LUA_RENDER_LAYER_ABOVE_BACKGROUND,
		LUA_RENDER_LAYER_ABOVE_PLAYERS,
		LUA_RENDER_LAYER_ABOVE_FOREGROUND,
		LUA_RENDER_LAYER_UNDER_HUD,
		LUA_RENDER_LAYER_ABOVE_HUD,
		LUA_RENDER_LAYER_ABOVE_MENUS,
		LUA_RENDER_LAYER_LAST,
		LUA_RENDER_LAYER_COUNT,
	};

	enum 
	{
		__LUA_RENDER = LUA_RENDER_LAYER_COUNT,
		LUA_RENDER_MENU,
	};

	enum
	{
		LUA_PREMISSION_NONE = 0,

		LUA_PREMISSION_READ = 1 << 0,
		LUA_PREMISSION_WRITE = 1 << 1,

		LUA_PREMISSION_READ_WRITE = LUA_PREMISSION_READ | LUA_PREMISSION_WRITE,

		LUA_PREMISSION_DDNET_ORG_CURL = 1 << 2,
		LUA_PREMISSION_CURL = 1 << 3 | LUA_PREMISSION_DDNET_ORG_CURL,

		LUA_PREMISSION_FOLDER_ASSETS = 1 << 4,
		LUA_PREMISSION_FOLDER_ROOT = 1 << 5,

		LUA_PREMISSION_ALL = LUA_PREMISSION_READ_WRITE | LUA_PREMISSION_CURL,
	};

protected:
	class CLuaBridge
	{
	public:
		struct CField 
		{
			template <typename T>
			static CField From(std::string_view Name, size_t Offset)
			{
				CField Field;
				Field.m_Name = Name;
				Field.m_StructId = typeid(T).hash_code();
				Field.m_Offset = Offset;
				Field.m_Size = sizeof(T);
				return Field;
			}
#define CFIELD(Name, Struct, Field) CLua::CLuaBridge::CField::From<decltype(Struct::Field)>(Name, offsetof(Struct, Field))

			std::string m_Name;
			size_t m_StructId;

			size_t m_Offset;
			size_t m_Size;
		};

		struct CArg
		{
			size_t m_StructId;

			size_t m_TypeHash;

			bool m_Reference;
			bool m_Pointer; 
		};

		struct CFunction {
			using ArgList = std::pair<std::vector<CArg>, std::function<int(const CLuaBridge*, lua_State*, void**)>>;

			template <typename ReturnType, typename... Args>
			static ArgList From(std::function<ReturnType(Args...)> Function);

			std::string m_Name;

			std::vector<ArgList> m_Args;

		private:
			template<typename ReturnType, typename... Args, size_t... Index>
			static ArgList FromImpl(std::function<ReturnType(Args...)> Function, std::index_sequence<Index...>);
		};

		struct CStruct 
		{
			std::string m_Name;

			std::vector<CField> m_Fields;
			std::unordered_map<size_t, CFunction> m_Methods;

			size_t m_Size;
			void (*m_Destructor)(void *);
		};

		struct CInvalidField
		{
			enum class EReason
			{
				None,
				MissingField,
				WrongType,
			};

			size_t m_StructId;
			size_t m_Index;
			std::unique_ptr<CInvalidField> m_pChild;

			EReason m_Reason;
		};

		template<typename ClassT>
		struct CMethod
		{
			template<typename ReturnType, typename... Args>
			CMethod(std::string_view Name, ReturnType (ClassT::*Method)(Args...));

			template<typename ReturnType, typename... Args>
			CMethod(std::string_view Name, ReturnType (ClassT::*Method)(Args...) const);

			template<typename ReturnType, typename... Args>
			CMethod(std::string_view Name, ReturnType (*Method)(ClassT &, Args...));

			template<typename ReturnType, typename... Args>
			CMethod(std::string_view Name, ReturnType (*Method)(const ClassT &, Args...));

			std::string m_Name;

			CFunction::ArgList m_Args;
		};
#define CMETHOD(Name, Struct, Method) CLua::CLuaBridge::CMethod<Struct>(Name, &Struct::Method)
#define CFASMETHOD(Name, Struct, Method) CLua::CLuaBridge::CMethod<Struct>(Name, Method)

		template<typename ClassT>
		struct CConstructor
		{
			CConstructor(std::string_view Name) : m_Name(Name) {}

			template<typename... Args>
			CConstructor(std::string_view Name, ClassT (*Constructor)(Args...));

			template<typename LambdaType>
			CConstructor(std::string_view Name, LambdaType Function);

			std::string m_Name;

			std::optional<CFunction::ArgList> m_Args;

		private:
			template<typename LambdaType, typename... Args>
			CConstructor(std::string_view Name, LambdaType Function, ClassT (LambdaType::*)(Args...) const); //TODO: test if non const is needed
		};
#define CCONSTRUCTOR(Name, Struct, Constructor) CLua::CLuaBridge::CConstructor<Struct>(Name, Constructor)

		struct CError
		{
			CError(std::string_view Message) :
				m_Message(Message)
			{}

			[[noreturn]] static void Throw(std::string_view Message) { throw CError(Message); }

			std::string m_Message;
		};

	public:
		CLuaBridge();
		CLuaBridge(std::string Path);
		CLuaBridge(std::string Path, std::string MetatablePath);

		CLuaBridge(const CLuaBridge &) = delete;
		CLuaBridge &operator=(const CLuaBridge &) = delete;
		CLuaBridge(CLuaBridge &&) = default;
		CLuaBridge &operator=(CLuaBridge &&) = default;

		template <typename T>
		void BridgeStruct(std::string_view Name, std::initializer_list<CField> Fields, std::initializer_list<CMethod<T>> Methods = {}, std::initializer_list<CConstructor<T>> Constructors = { CConstructor<T>("new") });

		template <typename LambdaType>
		void BridgeFunction(std::string_view Name, LambdaType Function);

		template <typename ReturnType, typename... Args>
		void BridgeFunction(std::string_view Name, ReturnType (*Function)(Args...));

		template <typename ReturnType, typename... ArgsT>
		void BridgeFunction(std::string_view Name, ReturnType (CLua::*Function)(ArgsT...))
		{
			BridgeFunction(Name, [Function](CLua *pLua, ArgsT... Args) { return (pLua->*Function)(Args...); });
		}

		template <typename... Args>
		void SetSuperArgs(Args... SuperArgs);

		void Setup(lua_State *pLuaState) const;

		template <typename T>
		T Check(lua_State *pLuaState, int Index) const;

		template <typename T>
		std::optional<T> Get(lua_State *pLuaState, int Index) const;

		template<typename T>
		std::variant<T, CInvalidField> GetI(lua_State *pLuaState, int Index) const;

		template <typename T>
		void Push(lua_State *pLuaState, const T &Value) const;

		template <typename T>
		void Set(lua_State *pLuaState, const T &Value, int Index) const;

		static void GetGlobal(lua_State *pLuaState, std::string_view Path);
		static void GetLocal(lua_State *pLuaState, std::string_view Path);
		static void GetField(lua_State *pLuaState, std::string_view Path, int Index);
		static std::string_view GetFieldTable(lua_State *pLuaState, std::string_view Path, int Index);
		static void SetField(lua_State *pLuaState, std::string_view Path, int Index); // TODO: meybe delete

		static void SetReadonlyMt(lua_State *pLuaState, int Index);

		std::string Format(const CInvalidField &Field) const;

	protected:
		const CStruct &GetStruct(size_t StructId) const;

		template <typename T>
		const CStruct &GetStruct() const { return GetStruct(typeid(T).hash_code()); }

		CFunction &GetFunction(std::string_view Name);

		static CFunction &GetFunction(std::string_view Name, std::unordered_map<size_t, CFunction> &Functions);

		void BridgeFunctionImpl(std::string_view Name, CFunction::ArgList Args);

		template<typename LambdaType, typename ReturnType, typename... Args>
		void BridgeFunctionImpl(std::string_view Name, LambdaType Function, ReturnType (LambdaType::*)(Args...) const); //TODO: test if non const is needed

		std::variant<void*, CInvalidField> Get(size_t StructId, lua_State *pLuaState, int Index) const;

		void Set(size_t StructId, lua_State *pLuaState, const void *pValue, int Index) const;

		void Push(size_t StructId, lua_State *pLuaState, const void *pValue) const;

		static bool IsFundamental(size_t StructId);

		std::string_view StructName(size_t StructId) const;

		[[noreturn]] void Error(lua_State *pLuaState, const std::string &Message) const;
		
		void Destruct(size_t StructId, void *pValue) const;

		template <bool IsMethod>
		static int Parser(lua_State *pLuaState);

		std::unordered_map<size_t, CStruct> m_Structs;
		std::unordered_map<size_t, CFunction> m_Functions;

		struct CFree
		{
			constexpr CFree() noexcept = default;

			void operator()(void *Ptr) const noexcept
			{
				free(Ptr);
			}
		};

		std::unordered_map<size_t, std::unique_ptr<void, CFree>> m_SuperArgs;

		std::string m_Path;
		std::string m_MetatablePath;
	};

	class CLuaSandbox
	{
	public:
		CLuaSandbox() = default;



		void Setup(lua_State *pLuaState) const;
	};

public:
	CLua();
	~CLua() override;

	int Sizeof() const override { return sizeof(*this); }

	static void ConLua(IConsole::IResult *pResult, void *pUserData);
	static void ConLuaExec(IConsole::IResult *pResult, void *pUserData);

	void OnConsoleInit() override;

	bool DoScript(const char *pScript);
	bool DoFile(const char *pFilename);

	void OnInit() override;

	void Init();
	void Close();
	bool Active() const { return m_pLuaState != nullptr; }


	void OnRender(int LuaRenderType);
	void OnRender(int LuaRenderType, CUIRect Box);

	CRenderLayer m_aRenderLayers[LUA_RENDER_LAYER_COUNT];
	
public:
	void LuaPrint(const std::string &Text);

	std::string LUAPathBound(std::string_view Path) const;

	void LuaWrite(std::string_view Path, std::string_view Text);
	void LuaAppend(std::string_view Path, std::string_view Text);
	std::string LuaRead(std::string_view Path);

protected:
	lua_State *m_pLuaState;

	CLuaBridge m_Bridge;
};


/*class CLua : public CComponent
{


public:


protected:
	enum EEventType
	{
		LUA_EVENT_TYPE_NONE = -1,
		LUA_EVENT_TYPE_ON_STATE_CHANGE = 0,
		LUA_EVENT_TYPE_ON_INIT,
		LUA_EVENT_TYPE_ON_SHUTDOWN,
		LUA_EVENT_TYPE_ON_RESET,
		LUA_EVENT_TYPE_ON_WINDOW_RESIZE,
		LUA_EVENT_TYPE_ON_REFRESH_SKINS,
		LUA_EVENT_TYPE_ON_RENDER,
		LUA_EVENT_TYPE_ON_NEW_SNAPSHOT,
		LUA_EVENT_TYPE_ON_RELEASE,
		LUA_EVENT_TYPE_ON_MAP_LOAD,
		LUA_EVENT_TYPE_ON_MESSAGE,
		LUA_EVENT_TYPE_ON_INPUT,
		LUA_EVENT_TYPE_COUNT
	};

public:
	CLua();
	~CLua() override;
	int Sizeof() const override { return sizeof(*this); }

	void InitLua();
	void CloseLua();

	int DoString(const char *pString);
	int DoFile(const char *pFilename);

	void OnConsoleInit() override;

	void OnStateChange(int NewState, int OldState) override;
	void OnInit() override;	
	void OnShutdown() override;
	void OnReset() override;
	void OnWindowResize() override;
	void OnRefreshSkins() override;
	//virtual void OnRender() override;
	void OnNewSnapshot() override;
	void OnRelease() override;
	void OnMapLoad() override;
	void OnMessage(int Msg, void *pRawMsg) override;
	bool OnInput(const IInput::CEvent &Event) override;

	inline bool Active() const { return m_pLuaState != nullptr; }

	void OnSendMessage(int Msg, void *pRawMsg);

public:
	void OnRenderLayer(int Layer, std::optional<CUIRect> Box = std::nullopt);
	void RenderMenu(CUIRect MainView);

	static void ConLua(IConsole::IResult *pResult, void *pUserData);
	static void ConExecLua(IConsole::IResult *pResult, void *pUserData);



protected:
	static CLua *GetSelf(lua_State *pLuaState);	

	static bool CLua::RunOnEvent(CLua *pLua, lua_State *pLuaState, CLua::EEventType EventType, int ArgsCount = 0, int ReturnCount = 0);

	static int LuaLoadFile(lua_State *pLuaState);
	static int LuaFindFile(lua_State *pLuaState);
	
	static int LuaPrint(lua_State *pLuaState);
	static int LuaDoFile(lua_State *pLuaState);

	static int LuaConsoleRunCommand(lua_State *pLuaState);

	static int LuaSetSetting(lua_State *pLuaState);
	static int LuaGetSetting(lua_State *pLuaState);
	static int LuaGetSettingInfo(lua_State *pLuaState);

	static int LuaGetGameTick(lua_State *pLuaState);
	static int LuaGetGameTickSpeed(lua_State *pLuaState);

	static int LuaGetClientData(lua_State *pLuaState);
	static int LuaGetClientStats(lua_State *pLuaState);
	static int LuaGetClientTeam(lua_State *pLuaState);

	static int LuaGetLocalId(lua_State *pLuaState);
	static int LuaGetLocalDummyId(lua_State *pLuaState);

	static int LuaGetLocalName(lua_State *pLuaState);
	static int LuaGetLocalDummyName(lua_State *pLuaState);

	static int LuaIsDummyControlled(lua_State *pLuaState);
	static int LuaToggleDummyControl(lua_State *pLuaState);

	static int LuaSendInfo(lua_State *pLuaState);
	static int LuaSendDummyInfo(lua_State *pLuaState);	

	static int LuaGetState(lua_State *pLuaState);
	
	static int LuaConnect(lua_State *pLuaState);
	static int LuaConnectDummy(lua_State *pLuaState);

	static int LuaDisconnect(lua_State *pLuaState);
	static int LuaDisconnectDummy(lua_State *pLuaState);

	static int LuaIsDummyAllowed(lua_State *pLuaState);
	static int LuaIsDummyConnected(lua_State *pLuaState);
	static int LuaIsDummyConnecting(lua_State *pLuaState);

	static int LuaQuit(lua_State *pLuaState);
	static int LuaRestart(lua_State *pLuaState);

	static int LuaKill(lua_State *pLuaState);

	static int LuaNewUIRect(lua_State *pLuaState);

	static int LuaGetScreenUIRect(lua_State *pLuaState);

	static int LuaMapScreen(lua_State *pLuaState);
	static int LuaPixelSize(lua_State *pLuaState);

	using UIRECTSPLITMID = void(CUIRect::*)(CUIRect *, CUIRect *, float) const;
	using UIREACTSPLITSIDE = void(CUIRect::*)(float, CUIRect *, CUIRect *) const;

	static int BasicLuaUIRectSplitMid(lua_State *pLuaState, UIRECTSPLITMID pfnCallback);
	static int BasicLuaUIRectSplitSide(lua_State *pLuaState, UIREACTSPLITSIDE pfnCallback);

	static int LuaUIRectMargin(lua_State *pLuaState);

	using UIRECTMARGIN = void(CUIRect::*)(float, CUIRect *) const;
	static int BasicLuaUIRectMargin(lua_State *pLuaState, UIRECTMARGIN pfnCallback);

	static int LuaUIRectDraw(lua_State *pLuaState);
	static int LuaUIRectDraw4(lua_State *pLuaState);

	static int LuaUIDoLabel(lua_State *pLuaState);

	static int LuaGetSnapshotNumberItems(lua_State *pLuaState);
	static int LuaGetSnapshotItem(lua_State *pLuaState);

	static CUIRect CheckUIRect(lua_State *pLuaState, int Index);
	static std::optional<CUIRect> GetUIRect(lua_State *pLuaState, int Index);
	static void SetUIRect(lua_State *pLuaState, const CUIRect &Rect, int Index);
	static void PushUIRect(lua_State *pLuaState, const CUIRect &Rect);

	static ColorHSLA CheckColor(lua_State *pLuaState, int Index);
	static std::optional<ColorHSLA> GetColor(lua_State *pLuaState, int Index);
	static void PushColor(lua_State *pLuaState, const ColorHSLA &Color, bool Alpha = true);

	static std::optional<vec2> GetVec2(lua_State *pLuaState, int Index);

	static std::optional<SLabelProperties> GetLabelProperties(lua_State *pLuaState, int Index);
	static std::optional<STextColorSplit> GetTextColorSplit(lua_State *pLuaState, int Index);

protected:
	lua_State *m_pLuaState;

	EEventType m_CurrentEvent;
	static const char *ms_apEventLuaNames[LUA_EVENT_TYPE_COUNT];

public:
	CLuaRenderLayer m_aRenderLayers[LUA_RENDER_LAYER_COUNT];
}; //*/

#endif