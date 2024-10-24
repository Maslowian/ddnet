#include "lua.h"
#include <engine/client.h>
#include <game/client/gameclient.h>
#include <type_traits>

#include <lua.hpp>

#include <optional>
#include <sstream>
#include <string_view>
#include <variant>
#include <tuple>

static const ColorRGBA gs_LuaMessageColor(0.63f, 0.78f, 1.0f);
static const ColorRGBA gs_LuaPrintColor(0.45f, 0.78f, 0.93f);
static const ColorRGBA gs_LuaErrorColor(0.97f, 0.f, 0.21f);

int CLua::CRenderLayer::Index() const
{
	return this - m_pClient->m_Lua.m_aRenderLayers;
}

void CLua::CRenderLayer::OnRender()
{
	m_pClient->m_Lua.OnRender(Index());
}

CLua::CLuaBridge::CLuaBridge() :
	CLuaBridge("_G")
{}

CLua::CLuaBridge::CLuaBridge(std::string Path) :
	CLuaBridge(Path, Path + ".structs")
{}

CLua::CLuaBridge::CLuaBridge(std::string Path, std::string MetatablePath) :
	m_Path(Path), m_MetatablePath(MetatablePath)
{}

template <typename T>
void CLua::CLuaBridge::BridgeStruct(std::string_view Name, std::initializer_list<CLuaBridge::CField> Fields, std::initializer_list<CMethod<T>> Methods, std::initializer_list<CConstructor<T>> Constructors)
{
	// TODO: static_assert for invalid types / not pure fileds

	size_t StructId = typeid(T).hash_code();

	dbg_assert(m_Structs.find(StructId) == m_Structs.end(), "Struct already bridged");
	for(const auto &Field : Fields)
	{
		dbg_assert(IsFundamental(Field.m_StructId) || m_Structs.find(Field.m_StructId) != m_Structs.end(), "Missing struct");
	}

	std::unordered_map<std::size_t, CFunction> MethodList;
	MethodList.reserve(Methods.size());

	for(auto &Method : Methods)
	{
		CFunction &Meth = GetFunction(Method.m_Name, MethodList);
		Meth.m_Args.push_back(std::move(Method.m_Args));
	}

	m_Structs.emplace(StructId, CStruct{std::string(Name), Fields, MethodList, sizeof(T), [](void *pValue) -> void { reinterpret_cast<T *>(pValue)->~T(); }});
}

template<typename LambdaType>
void CLua::CLuaBridge::BridgeFunction(std::string_view Name, LambdaType Function)
{
	// TODO: static_assert(std::is_invocable_v<LambdaType>);
	BridgeFunctionImpl<LambdaType>(Name, Function, &LambdaType::operator());
}

template<typename LambdaType, typename ReturnType, typename... Args>
void CLua::CLuaBridge::BridgeFunctionImpl(std::string_view Name, LambdaType Function, ReturnType (LambdaType::*)(Args...) const)
{
	BridgeFunctionImpl(Name, CFunction::From(std::function<ReturnType(Args...)>(Function)));
}

template<typename ReturnType, typename... Args>
void CLua::CLuaBridge::BridgeFunction(std::string_view Name, ReturnType (*Function)(Args...))
{
	BridgeFunctionImpl(Name, CFunction::From(std::function<ReturnType(Args...)>(Function)));
}

void CLua::CLuaBridge::BridgeFunctionImpl(std::string_view Name, CFunction::ArgList Args)
{
	CFunction &Func = GetFunction(Name);

	Func.m_Args.push_back(std::move(Args));
}

// From => To, IsReference(CanChange), IsPointer
// T => T, false, false
// const T => const T, false, false
// cosnt T& => const T, false, false
// T& => T, true, false
// const T* => const T*, false, true
// T* => T*, true, true

template <typename ReturnType, typename... Args>
CLua::CLuaBridge::CFunction::ArgList CLua::CLuaBridge::CFunction::From(std::function<ReturnType(Args...)> Function)
{
	return FromImpl<ReturnType, Args...>(Function, std::index_sequence_for<Args...>{});
}

template <typename ReturnType, typename... Args, size_t... Index>
CLua::CLuaBridge::CFunction::ArgList CLua::CLuaBridge::CFunction::FromImpl(std::function<ReturnType(Args...)> Function, std::index_sequence<Index...>)
{
	// TODO: static_assert if T and ReturnT without rpcv is accpetable struct/FUNDAMENTAL

	std::vector<CArg> FuncArgs;
	FuncArgs.reserve(sizeof...(Args));
	((FuncArgs.push_back(CArg{typeid(std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<Args>>>).hash_code(), typeid(Args).hash_code(), !std::is_const_v<std::remove_pointer_t<std::remove_reference_t<Args>>> && (std::is_pointer_v<Args> || std::is_reference_v<Args>), std::is_pointer_v<Args>})), ...);

	ArgList::second_type FuncFunction = [Function](const CLuaBridge *pBridge, lua_State *pLuaState, void **aArgs) -> int 
	{
		if constexpr(std::is_void_v<ReturnType>)
		{
			try
			{
				std::invoke(Function, (*reinterpret_cast<std::remove_reference_t<std::tuple_element_t<Index, std::tuple<Args...>>> *>(aArgs[Index]))...);
			}
			catch(const CError& Error)
			{
				lua_pushstring(pLuaState, Error.m_Message.c_str());
				return -1;
			}
			return 0;
		}
		else
		{
			ReturnType Result;
			try
			{
				Result = std::invoke(Function, (*reinterpret_cast<std::remove_reference_t<std::tuple_element_t<Index, std::tuple<Args...>>> *>(aArgs[Index]))...);
			}
			catch(const CError &Error)
			{
				lua_pushstring(pLuaState, Error.m_Message.c_str());
				return -1;
			}

			if constexpr(std::is_pointer_v<ReturnType>)
			{
				if(Result)
				{
					pBridge->Push(pLuaState, *Result);
					delete Result;
					return 1;
				}

				lua_pushnil(pLuaState);
				return 1;
			}
			else
			{
				pBridge->Push(pLuaState, Result);
				return 1;
			}
		}
	};

	return {std::move(FuncArgs), FuncFunction};
}


template<typename ClassT>
template<typename ReturnType, typename... Args>
inline CLua::CLuaBridge::CMethod<ClassT>::CMethod(std::string_view Name, ReturnType (ClassT::*Method)(Args...))
{
	m_Name = Name;

	m_Args = CFunction::From(std::function<ReturnType(ClassT&, Args...)>([Method](ClassT &This, Args... Args) -> ReturnType { return (This.*Method)(Args...); }));
}

template<typename ClassT>
template<typename ReturnType, typename... Args>
inline CLua::CLuaBridge::CMethod<ClassT>::CMethod(std::string_view Name, ReturnType (ClassT::*Method)(Args...) const)
{
	m_Name = Name;

	m_Args = CFunction::From(std::function<ReturnType(const ClassT &, Args...)>([Method](const ClassT &This, Args... Args) -> ReturnType { return (This.*Method)(Args...); }));
}

template<typename ClassT>
template<typename ReturnType, typename... Args>
CLua::CLuaBridge::CMethod<ClassT>::CMethod(std::string_view Name, ReturnType (*Method)(ClassT &, Args...))
{
	m_Name = Name;

	m_Args = CFunction::From(std::function<ReturnType(ClassT &, Args...)>(Method));
}

template<typename ClassT>
template<typename ReturnType, typename... Args>
CLua::CLuaBridge::CMethod<ClassT>::CMethod(std::string_view Name, ReturnType (*Method)(const ClassT &, Args...))
{
	m_Name = Name;

	m_Args = CFunction::From(std::function<ReturnType(const ClassT &, Args...)>(Method));
}

template<typename ClassT>
template<typename... Args>
CLua::CLuaBridge::CConstructor<ClassT>::CConstructor(std::string_view Name, ClassT (*Constructor)(Args...))
{
	m_Name = Name;

	m_Args = CFunction::From(std::function<ClassT(Args...)>(Constructor));
}

template<typename ClassT>
template<typename LambdaType>
CLua::CLuaBridge::CConstructor<ClassT>::CConstructor(std::string_view Name, LambdaType Constructor) :
	CConstructor(Name, Constructor, &LambdaType::operator())
{
	// TODO: static_assert(std::is_invocable_v<LambdaType>);
	static_assert(std::is_same_v<ClassT, std::remove_cv_t<std::remove_reference_t<std::invoke_result_t<LambdaType>>>>);
}

template<typename ClassT>
template<typename LambdaType, typename... Args>
CLua::CLuaBridge::CConstructor<ClassT>::CConstructor(std::string_view Name, LambdaType Constructor, ClassT (LambdaType::*)(Args...) const)
{
	m_Name = Name;

	m_Args = CFunction::From(std::function<ClassT(Args...)>(Constructor));
}

CLua::CLuaBridge::CFunction &CLua::CLuaBridge::GetFunction(std::string_view Name)
{
	return GetFunction(Name, m_Functions);
}

CLua::CLuaBridge::CFunction &CLua::CLuaBridge::GetFunction(std::string_view Name, std::unordered_map<size_t, CFunction> &Functions)
{
	size_t Hash = std::hash<std::string_view>{}(Name);

	auto It = Functions.find(Hash);

	if(It == Functions.end())
	{
		It = Functions.emplace(Hash, CFunction{std::string(Name)}).first;
	}

	return It->second;
}

template<typename... Args>
void CLua::CLuaBridge::SetSuperArgs(Args... SuperArgs)
{
	(([&]() {
		void *pValue = new uint8_t[sizeof(Args)];

		memcpy(pValue, &SuperArgs, sizeof(Args));

		m_SuperArgs.insert_or_assign(typeid(Args).hash_code(), std::unique_ptr<void, CFree>(pValue));
	}()), ...);
}

void CLua::CLuaBridge::Setup(lua_State *pLuaState) const
{
	static_assert(sizeof(void *) >= sizeof(size_t));

	GetGlobal(pLuaState, m_MetatablePath);

	for (const auto &Struct : m_Structs)
	{
		lua_newtable(pLuaState);

		lua_newtable(pLuaState);

		for(const auto &Method : Struct.second.m_Methods)
		{
			lua_pushlightuserdata(pLuaState, (void *)this);
			lua_pushlightuserdata(pLuaState, (void *)Method.first);
			lua_pushlightuserdata(pLuaState, (void *)Struct.first);
			lua_pushcclosure(pLuaState, Parser<true>, 3);
			SetField(pLuaState, Method.second.m_Name, -2);
		}

		SetReadonlyMt(pLuaState, -1);

		lua_setfield(pLuaState, -2, "__index");

		// TODO: add constructors here

		SetReadonlyMt(pLuaState, -1);

		SetField(pLuaState, Struct.second.m_Name, -2);
	}

	lua_pop(pLuaState, 1);

	GetGlobal(pLuaState, m_Path);

	for (const auto& Function : m_Functions)
	{
		lua_pushlightuserdata(pLuaState, (void *)this);
		lua_pushlightuserdata(pLuaState, (void *)Function.first);
		lua_pushcclosure(pLuaState, Parser<false>, 2);
		SetField(pLuaState, Function.second.m_Name, -2);
	}

	lua_pop(pLuaState, 1);
}

template<bool IsMethod>
int CLua::CLuaBridge::Parser(lua_State *pLuaState)
{
	const CLuaBridge *const pBridge = (CLuaBridge *)lua_touserdata(pLuaState, lua_upvalueindex(1));
	const size_t FunctionId = (size_t)lua_touserdata(pLuaState, lua_upvalueindex(2));

	const CFunction *pFunc;

	if constexpr (IsMethod)
	{
		size_t StructId = (size_t)lua_touserdata(pLuaState, lua_upvalueindex(3));

		const CStruct &Struct = pBridge->GetStruct(StructId);

		auto It = Struct.m_Methods.find(FunctionId);
		dbg_assert(It != Struct.m_Methods.end(), "Missing method");
		pFunc = &It->second;
	}
	else
	{
		auto It = pBridge->m_Functions.find(FunctionId);
		dbg_assert(It != pBridge->m_Functions.end(), "Missing function");
		pFunc = &It->second;
	}

	const auto IsSuper = [&](size_t Hash){ return Hash == typeid(lua_State*).hash_code() || pBridge->m_SuperArgs.find(Hash) != pBridge->m_SuperArgs.end(); };

	std::list<std::pair<const CFunction::ArgList *, size_t>> ValidArgLists;
	std::for_each(pFunc->m_Args.begin(), pFunc->m_Args.end(), [&](const CFunction::ArgList &ArgList) { ValidArgLists.push_back(std::make_pair(&ArgList, 0)); });

	const size_t ArgCount = lua_gettop(pLuaState);

	std::vector<std::pair<void *, const CArg *>> Values;
	Values.reserve(ArgCount);

	const auto FreeValues = [&]() 
	{
		for(auto &Value : Values)
		{
			if(!Value.second)
				continue;

			pBridge->Destruct(Value.second->m_StructId, Value.first);
			free(Value.first);
		}
		Values.clear();
	};

	void * pNullptr = nullptr;

	for (size_t ArgIndex = 0; ArgIndex < ArgCount; ArgIndex++)
	{
		const bool IsNil = lua_isnil(pLuaState, ArgIndex + 1);

		std::optional<size_t> CurrentArgType;

		const size_t ValidArgListsCount = ValidArgLists.size();

		for(auto ArgListIt = ValidArgLists.begin(); ArgListIt != ValidArgLists.end();)
		{
			size_t &RealIndex = ArgListIt->second;
			const std::vector<CArg> &Args = ArgListIt->first->first;

			const CArg *pCurrentArg = nullptr;

			do
			{
				if(RealIndex >= Args.size())
					break;

				const CArg &Arg = Args[RealIndex];

				if(IsSuper(Arg.m_TypeHash))
				{
					RealIndex++;
					continue;
				}

				pCurrentArg = &Arg;
				RealIndex++;
				break;
			}
			while(true);

			if(!pCurrentArg)
			{
				ArgListIt = ValidArgLists.erase(ArgListIt);
				continue;
			}

			if(IsNil && ValidArgListsCount != 1)
			{
				if(!pCurrentArg->m_Pointer)
				{
					ArgListIt = ValidArgLists.erase(ArgListIt);
					continue;
				}

				if (CurrentArgType)
				{
					Values.emplace_back(&pNullptr, nullptr);
					CurrentArgType = 0;
				}
				
				ArgListIt++;
				continue;
			}

			if (CurrentArgType)
			{
				if(pCurrentArg->m_StructId != *CurrentArgType)
				{
					ArgListIt = ValidArgLists.erase(ArgListIt);
					continue;
				}

				ArgListIt++;
				continue;
			}

			auto Result = pBridge->Get(pCurrentArg->m_StructId, pLuaState, ArgIndex + 1);

			if(auto *InvalidField = std::get_if<CInvalidField>(&Result))
			{
				if (ValidArgListsCount == 1)
				{
					FreeValues();

					std::string Message;

					if (IsMethod && ArgIndex == 0)
					{
						Message = "Invalid object";
					}
					else
					{
						Message = "Invalid argument ";
						Message += std::to_string(ArgIndex + 1 - IsMethod);
					}
					Message += ": ";
					Message += pBridge->Format(*InvalidField);
					
					pBridge->Error(pLuaState, Message);
				}

				ArgListIt = ValidArgLists.erase(ArgListIt);
				continue;
			}

			Values.emplace_back(std::get<void *>(Result), pCurrentArg);
			CurrentArgType = pCurrentArg->m_StructId;

			ArgListIt++;
		}
	}

	for (auto ArgListIt = ValidArgLists.begin(); ArgListIt != ValidArgLists.end();)
	{
		size_t &RealIndex = ArgListIt->second;
		const std::vector<CArg> &Args = ArgListIt->first->first;
		
		while (RealIndex < Args.size())
		{
			if(IsSuper(Args[RealIndex].m_TypeHash))
			{
				RealIndex++;
				continue;
			}

			if (!Args[RealIndex++].m_Pointer)
			{
				ArgListIt = ValidArgLists.erase(ArgListIt);
				goto next;
			}
		}

		ArgListIt++;
	next:;
	}

	if(ValidArgLists.size() < 1)
	{
		FreeValues();
		if constexpr(IsMethod)
			pBridge->Error(pLuaState, "No method match");
		else
			pBridge->Error(pLuaState, "No function match");
	}
	else if(ValidArgLists.size() > 1)
	{
		FreeValues();
		if constexpr(IsMethod)
			pBridge->Error(pLuaState, "More than one method match");
		else
			pBridge->Error(pLuaState, "More than one function match");
	}

	const CFunction::ArgList *pArgList = ValidArgLists.front().first;

	const CFunction::ArgList::first_type &ArgList = pArgList->first;
	const CFunction::ArgList::second_type &Function = pArgList->second;

	std::vector<void *> Args;
	Args.reserve(ArgList.size());

	size_t ValueIndex = 0;
	for(const auto &Arg : ArgList)
	{
		if(typeid(lua_State *).hash_code() == Arg.m_TypeHash)
		{
			Args.push_back(&pLuaState);
			continue;
		}
		else if(auto It = pBridge->m_SuperArgs.find(Arg.m_TypeHash); It != pBridge->m_SuperArgs.end())
		{
			Args.push_back(It->second.get());
			continue;
		}

		if(Arg.m_Pointer)
		{
			if (ValueIndex >= Values.size())
			{
				Args.push_back(&pNullptr);
			}
			else
			{
				Args.push_back(&Values[ValueIndex++].first);
			}
		}
		else
		{
			Args.push_back(Values[ValueIndex++].first);
		}
	}

	auto Result = Function(pBridge, pLuaState, Args.data());

	FreeValues();

	if(Result == -1)
		lua_error(pLuaState);

	return Result;
}

template<typename T>
T CLua::CLuaBridge::Check(lua_State *pLuaState, int Index) const
{
	auto Result = Get(typeid(T).hash_code(), pLuaState, Index);

	if(auto *InvalidField = std::get_if<CInvalidField>(&Result))
		return Error(pLuaState, Format(*InvalidField));

	void *pValue = std::get<void *>(Result);

	T Value;

	memcpy(&Value, pValue, sizeof(T));

	free(pValue);

	return Value;
}

template<typename T>
std::variant<T, CLua::CLuaBridge::CInvalidField> CLua::CLuaBridge::GetI(lua_State* pLuaState, int Index) const
{
	auto Result = Get(typeid(T).hash_code(), pLuaState, Index);

	if(std::holds_alternative<CInvalidField>(Result))
		return std::get<CInvalidField>(Result);

	void *pValue = std::get<void *>(Result);

	T Value;

	memcpy(&Value, pValue, sizeof(T));

	free(pValue);

	return Value;
}

template <typename T>
std::optional<T> CLua::CLuaBridge::Get(lua_State *pLuaState, int Index) const
{
	auto Result = Get(typeid(T).hash_code(), pLuaState, Index);

	if(std::holds_alternative<CInvalidField>(Result))
		return std::nullopt;

	void *pValue = std::get<void *>(Result);

	T Value;

	memcpy(&Value, pValue, sizeof(T));

	free(pValue);

	return Value;
}

template <typename T>
void CLua::CLuaBridge::Set(lua_State *pLuaState, const T &Value, int Index) const
{
	Set(typeid(T).hash_code(), pLuaState, &Value, Index);
}

template <typename T>
void CLua::CLuaBridge::Push(lua_State *pLuaState, const T &Value) const
{
	Push(typeid(T).hash_code(), pLuaState, &Value);
}

#define luaL_checkboolean(L, n) (luaL_checktype(L, n, LUA_TBOOLEAN), lua_toboolean(L, n))

std::variant<void *, CLua::CLuaBridge::CInvalidField> CLua::CLuaBridge::Get(size_t StructId, lua_State *pLuaState, int Index) const
{
	if(typeid(int).hash_code() == StructId)
	{
		if (!lua_isinteger(pLuaState, Index))
			return CInvalidField{StructId, 0, nullptr, CInvalidField::EReason::WrongType};

		return new int(static_cast<int>(lua_tointeger(pLuaState, Index)));
	}
	else if (typeid(long).hash_code() == StructId)
	{
		if(!lua_isinteger(pLuaState, Index))
			return CInvalidField{StructId, 0, nullptr, CInvalidField::EReason::WrongType};

		return new long(static_cast<long>(lua_tointeger(pLuaState, Index)));
	}
	else if(typeid(long long).hash_code() == StructId)
	{
		if(!lua_isinteger(pLuaState, Index))
			return CInvalidField{StructId, 0, nullptr, CInvalidField::EReason::WrongType};

		return new long long(static_cast<long long>(lua_tointeger(pLuaState, Index)));
	}
	else if(typeid(float).hash_code() == StructId)
	{
		if (!lua_isnumber(pLuaState, Index))
			return CInvalidField{StructId, 0, nullptr, CInvalidField::EReason::WrongType};

		return new float(static_cast<float>(lua_tonumber(pLuaState, Index)));
	}
	else if(typeid(double).hash_code() == StructId)
	{
		if(!lua_isnumber(pLuaState, Index))
			return CInvalidField{StructId, 0, nullptr, CInvalidField::EReason::WrongType};

		return new double(static_cast<double>(lua_tonumber(pLuaState, Index)));
	}
	else if(typeid(long double).hash_code() == StructId)
	{
		if(!lua_isnumber(pLuaState, Index))
			return CInvalidField{StructId, 0, nullptr, CInvalidField::EReason::WrongType};

		return new long double(static_cast<long double>(lua_tonumber(pLuaState, Index)));
	}
	else if (typeid(bool).hash_code() == StructId)
	{
		if (!lua_isboolean(pLuaState, Index))
			return CInvalidField{StructId, 0, nullptr, CInvalidField::EReason::WrongType};

		return new bool(lua_toboolean(pLuaState, Index));
	}
	else if (typeid(std::string_view).hash_code() == StructId)
	{
		if (!lua_isstring(pLuaState, Index))
			return CInvalidField{StructId, 0, nullptr, CInvalidField::EReason::WrongType};

		return new std::string_view(lua_tostring(pLuaState, Index));
	}
	else if (typeid(std::string).hash_code() == StructId)
	{
		if (!lua_isstring(pLuaState, Index))
			return CInvalidField{StructId, 0, nullptr, CInvalidField::EReason::WrongType};

		return new std::string(lua_tostring(pLuaState, Index));
	}

	const CStruct &Struct = GetStruct(StructId);

	if (!lua_istable(pLuaState, Index)) // TODO: check metatable
	{
		return CInvalidField{StructId, 0, nullptr, CInvalidField::EReason::WrongType};
	}

	void* pValue = new uint8_t(Struct.m_Size);

	for(size_t i = 0; i < Struct.m_Fields.size(); i++)
	{
		const auto &Field = Struct.m_Fields[i];

		if (lua_getfield(pLuaState, Index, Field.m_Name.c_str()) == LUA_TNIL)
		{
			for(size_t j = 0; j < i; j++)
			{
				const auto &Field = Struct.m_Fields[j];
				Destruct(Field.m_StructId, reinterpret_cast<uint8_t *>(pValue) + Field.m_Offset);
			}
			free(pValue);
			return CInvalidField{StructId, i + 1, nullptr, CInvalidField::EReason::MissingField};
		}

		auto pFieldValue = Get(Field.m_StructId, pLuaState, -1);
		if(auto *InvalidField = std::get_if<CInvalidField>(&pFieldValue))
		{
			for(size_t j = 0; j < i; j++)
			{
				const auto &Field = Struct.m_Fields[j];
				Destruct(Field.m_StructId, reinterpret_cast<uint8_t *>(pValue) + Field.m_Offset);
			}
			free(pValue);
			return CInvalidField{StructId, i + 1, std::make_unique<CInvalidField>(std::move(*InvalidField)), CInvalidField::EReason::None};
		}

		memcpy(reinterpret_cast<uint8_t *>(pValue) + Field.m_Offset, std::get<void*>(pFieldValue), Field.m_Size);

		free(std::get<void*>(pFieldValue));

		lua_pop(pLuaState, 1);
	}

	return pValue;
}

void CLua::CLuaBridge::Set(size_t StructId, lua_State *pLuaState, const void *pValue, int Index) const
{
	Index = Index < 0 ? lua_gettop(pLuaState) + Index + 1 : Index;

	if (IsFundamental(StructId))
	{
		Push(StructId, pLuaState, pValue);
		lua_replace(pLuaState, Index);
		return;
	}

	if (!lua_istable(pLuaState, Index)) // TODO: check metatable
	{
		Push(StructId, pLuaState, pValue);
		lua_replace(pLuaState, Index);
		return;
	}

	const CStruct &Struct = GetStruct(StructId);

	for (const auto &Field : Struct.m_Fields)
	{
		Push(Field.m_StructId, pLuaState, reinterpret_cast<const uint8_t *>(pValue) + Field.m_Offset);
		lua_setfield(pLuaState, Index, Field.m_Name.c_str());
	}
}

void CLua::CLuaBridge::Push(size_t StructId, lua_State *pLuaState, const void *pValue) const
{
	if(typeid(int).hash_code() == StructId)
	{
		lua_pushinteger(pLuaState, static_cast<LUA_INTEGER>(*reinterpret_cast<const int *>(pValue)));
		return;
	}
	if(typeid(long).hash_code() == StructId)
	{
		lua_pushinteger(pLuaState, static_cast<LUA_INTEGER>(*reinterpret_cast<const long *>(pValue)));
		return;
	}
	if(typeid(long long).hash_code() == StructId)
	{
		lua_pushinteger(pLuaState, static_cast<LUA_INTEGER>(*reinterpret_cast<const long long *>(pValue)));
		return;
	}
	else if(typeid(float).hash_code() == StructId)
	{
		lua_pushnumber(pLuaState, static_cast<LUA_NUMBER>(*reinterpret_cast<const float *>(pValue)));
		return;
	}
	else if(typeid(double).hash_code() == StructId)
	{
		lua_pushnumber(pLuaState, static_cast<LUA_NUMBER>(*reinterpret_cast<const double *>(pValue)));
		return;
	}
	else if(typeid(long double).hash_code() == StructId)
	{
		lua_pushnumber(pLuaState, static_cast<LUA_NUMBER>(*reinterpret_cast<const long double *>(pValue)));
		return;
	}
	else if (typeid(bool).hash_code() == StructId)
	{
		lua_pushboolean(pLuaState, *reinterpret_cast<const bool *>(pValue));
		return;
	}
	else if (typeid(std::string_view).hash_code() == StructId)
	{
		std::string String(*reinterpret_cast<const std::string_view *>(pValue));
		lua_pushstring(pLuaState, String.c_str());
		return;
	}
	else if (typeid(std::string).hash_code() == StructId)
	{
		lua_pushstring(pLuaState, reinterpret_cast<const std::string *>(pValue)->c_str());
		return;
	}

	const CStruct &Struct = GetStruct(StructId);

	lua_newtable(pLuaState);

	GetGlobal(pLuaState, m_MetatablePath + '.' + Struct.m_Name);
	lua_setmetatable(pLuaState, -2);

	Set(StructId, pLuaState, pValue, -1);
}


std::string CLua::CLuaBridge::Format(const CInvalidField &Field) const
{
	std::string Path;

	CInvalidField::EReason Reason;

	const CInvalidField *pField = &Field;

	while(true)
	{
		if(pField->m_Index)
		{
			const CStruct &Struct = GetStruct(pField->m_StructId);

			Path += ".";

			Path += Struct.m_Fields[pField->m_Index - 1].m_Name;

			if (pField->m_pChild)
			{
				Path += '[';
				Path += StructName(pField->m_pChild->m_StructId);
				Path += ']';
			}
		}

		if(!pField->m_pChild)
		{
			Reason = pField->m_Reason;
			break;
		}

		pField = pField->m_pChild.get();
	}

	std::string Type;
	Type += '[';
	Type += StructName(pField->m_StructId);
	Type += ']';

	if(pField != &Field)
		Path = std::string(StructName(Field.m_StructId)) + Path;

	std::string Message;

	switch(Reason)
	{
	case CInvalidField::EReason::None:
	{
		Message = "Unknown error";
		break;
	}
	case CInvalidField::EReason::MissingField:
	{
		Message += "Filed ";
		Message += Path;
		Message += Type;
		Message += " is missing";
		break;
	}
	case CInvalidField::EReason::WrongType:
	{
		if(Path.empty())
		{
			Message += "Wrong type, expected ";
			Message += Type;
		}
		else
		{
			Message += Path;
			Message += " have wrong type, expected ";
			Message += Type;
		}
		break;
	}
	}

	return Message;
}

const CLua::CLuaBridge::CStruct &CLua::CLuaBridge::GetStruct(size_t StructId) const
{
	auto it = m_Structs.find(StructId);
	dbg_assert(it != m_Structs.end(), "Missing struct");
	return it->second;
}

bool CLua::CLuaBridge::IsFundamental(size_t StructId)
{
	return typeid(int).hash_code() == StructId ||
	       typeid(long).hash_code() == StructId ||
	       typeid(long long).hash_code() == StructId ||
	       typeid(float).hash_code() == StructId ||
	       typeid(double).hash_code() == StructId ||
	       typeid(long double).hash_code() == StructId ||
	       typeid(bool).hash_code() == StructId ||
	       typeid(std::string_view).hash_code() == StructId ||
	       typeid(std::string).hash_code() == StructId;
}

std::string_view CLua::CLuaBridge::StructName(size_t StructId) const
{
	if(typeid(LUA_INTEGER).hash_code() == StructId)
	{
		return "Integer";
	}
	else if(typeid(LUA_NUMBER).hash_code() == StructId)
	{
		return "Number";
	}
	else if(typeid(bool).hash_code() == StructId)
	{
		return "Boolean";
	}
	else if(typeid(std::string_view).hash_code() == StructId)
	{
		return "String";
	}
	else if(typeid(std::string).hash_code() == StructId)
	{
		return "String";
	}

	const CStruct &Struct = GetStruct(StructId);

	return Struct.m_Name;
}

[[noreturn]] void CLua::CLuaBridge::Error(lua_State *pLuaState, const std::string &Message) const
{
	lua_pushstring(pLuaState, Message.c_str());
	lua_error(pLuaState);
	std::abort();
}

void CLua::CLuaBridge::Destruct(size_t StructId, void *pValue) const
{
	if(IsFundamental(StructId))
	{
		if (typeid(std::string_view).hash_code() == StructId)
		{
			reinterpret_cast<std::string_view *>(pValue)->~basic_string_view();
		}
		else if(typeid(std::string).hash_code() == StructId)
		{
			reinterpret_cast<std::string *>(pValue)->~basic_string();
		}

		return;
	}
	
	const CStruct &Struct = GetStruct(StructId);

	Struct.m_Destructor(pValue);
}

void CLua::CLuaBridge::GetGlobal(lua_State *pLuaState, std::string_view Path)
{
	std::string_view::size_type Start = 0;
	std::string_view::size_type End = Path.find_first_of('.');

	std::string Buffer(Path.substr(Start, End));

	if (lua_getglobal(pLuaState, Buffer.c_str()) == LUA_TNIL)
	{
		lua_pop(pLuaState, 1);
		lua_newtable(pLuaState);
		lua_pushvalue(pLuaState, -1);
		lua_setglobal(pLuaState, Buffer.c_str());
	}

	if(End == std::string_view::npos)
		return;

	GetField(pLuaState, Path.substr(End + 1), -1);
	lua_remove(pLuaState, -2);
}

void CLua::CLuaBridge::GetField(lua_State *pLuaState, std::string_view Path, int Index)
{
	Index = Index < 0 ? lua_gettop(pLuaState) + Index + 1 : Index;

	std::string_view::size_type Start = 0;
	std::string_view::size_type End = -1;

	std::string Buffer;

	bool MyField = false;

	do
	{
		Start = End + 1;
		End = Path.find_first_of('.', Start);

		Buffer = Path.substr(Start, End - Start);

		if(lua_getfield(pLuaState, MyField ? -1 : Index, Buffer.c_str()) == LUA_TNIL)
		{
			lua_pop(pLuaState, 1);
			lua_newtable(pLuaState);
			lua_pushvalue(pLuaState, -1);
			lua_setfield(pLuaState, MyField ? -3 : Index, Buffer.c_str());
		}

		if (MyField)
			lua_remove(pLuaState, -2);

		MyField = true;
	} while(End != std::string_view::npos);
}

std::string_view CLua::CLuaBridge::GetFieldTable(lua_State *pLuaState, std::string_view Path, int Index)
{
	Index = Index < 0 ? lua_gettop(pLuaState) + Index + 1 : Index;

	std::string_view::size_type Start = 0;
	std::string_view::size_type End = -1;

	std::string Buffer;

	bool MyField = false;

	do
	{
		Start = End + 1;
		End = Path.find_first_of('.', Start);

		if(End == std::string_view::npos)
		{
			if (!MyField)
				lua_pushvalue(pLuaState, Index);
			return Path.substr(Start);
		}

		Buffer = Path.substr(Start, End - Start);

		if(lua_getfield(pLuaState, MyField ? -1 : Index, Buffer.c_str()) == LUA_TNIL)
		{
			lua_pop(pLuaState, 1);
			lua_newtable(pLuaState);
			lua_pushvalue(pLuaState, -1);
			lua_setfield(pLuaState, MyField ? -3 : Index, Buffer.c_str());
		}

		if(MyField)
			lua_remove(pLuaState, -2);

		MyField = true;
	} while(true);
}

void CLua::CLuaBridge::SetField(lua_State *pLuaState, std::string_view Path, int Index)
{
	Index = Index < 0 ? lua_gettop(pLuaState) + Index + 1 : Index;

	std::string_view::size_type Start = 0;
	std::string_view::size_type End = -1;

	std::string Buffer;

	bool MyField = false;

	do
	{
		Start = End + 1;
		End = Path.find_first_of('.', Start);

		Buffer = Path.substr(Start, End - Start);

		if(End == std::string_view::npos)
		{
			if (!MyField)
			{
				lua_setfield(pLuaState, Index, Buffer.c_str());
			}
			else
			{
				lua_pushvalue(pLuaState, -2);
				lua_setfield(pLuaState, -2, Buffer.c_str());
				lua_pop(pLuaState, 2);
			}
			return;
		}


		if(lua_getfield(pLuaState, MyField ? -1 : Index, Buffer.c_str()) == LUA_TNIL)
		{
			lua_pop(pLuaState, 1);
			lua_newtable(pLuaState);
			lua_pushvalue(pLuaState, -1);
			lua_setfield(pLuaState, MyField ? -3 : Index, Buffer.c_str());
		}

		if(MyField)
			lua_remove(pLuaState, -2);

		MyField = true;
	} while(true);
}

void CLua::CLuaBridge::SetReadonlyMt(lua_State *pLuaState, int Index)
{
	Index = Index < 0 ? lua_gettop(pLuaState) + Index + 1 : Index;

	lua_newtable(pLuaState);

	lua_newtable(pLuaState);

	lua_pushvalue(pLuaState, Index);
	lua_setfield(pLuaState, -2, "__index");

	lua_pushcfunction(pLuaState, [](lua_State *pLuaState) -> int {
		luaL_error(pLuaState, "Attempt to modify a read-only table");
		return 0;
	});
	lua_setfield(pLuaState, -2, "__newindex");

	lua_pushboolean(pLuaState, true);
	lua_setfield(pLuaState, -2, "__metatable");

	lua_setmetatable(pLuaState, -2);

	lua_replace(pLuaState, Index);
}

CLua::CLua() :
	m_pLuaState(nullptr), m_Bridge("ddnet")
{}

CLua::~CLua()
{
	if(Active())
		Close();
}

void CLua::ConLua(IConsole::IResult *pResult, void *pUserData)
{
	CLua *pLua = (CLua *)pUserData;

	const char *pScript = pResult->GetString(0);

	pLua->DoScript(pScript);
}

void CLua::ConLuaExec(IConsole::IResult *pResult, void *pUserData)
{
	CLua *pLua = (CLua *)pUserData;

	const char *pFile = pResult->GetString(0);

	pLua->DoFile(pFile);
}

void CLua::OnConsoleInit()
{
	Console()->Register("lua", "s[script]", CFGFLAG_CLIENT, ConLua, this, "Run lua script");
	Console()->Register("lua_exec", "s[file]", CFGFLAG_CLIENT, ConLuaExec, this, "Run lua file");
}

bool CLua::DoScript(const char *pScript)
{
	if(!Active())
		Init();

	if(luaL_dostring(m_pLuaState, pScript) != LUA_OK)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "lua-error", lua_tostring(m_pLuaState, -1), gs_LuaErrorColor);
		lua_pop(m_pLuaState, 1);
		return false;
	}
	return true;
}

bool CLua::DoFile(const char *pFilename)
{
	if(!Active())
		Init();

	return false;
}

void test1()
{
	return;
}

int *test2(LUA_INTEGER *test, bool &a, LUA_INTEGER b)
{
	return new int(123);
}

float test3(const LUA_INTEGER &test)
{
	return 0.2137;
}

std::string *test4(const LUA_INTEGER *test, bool a)
{
	if(a)
		return new std::string("RARE");
	return nullptr;
}

void CLua::OnInit()
{
	m_Bridge.SetSuperArgs(this);

	m_Bridge.BridgeFunction("print", &CLua::LuaPrint);

	m_Bridge.BridgeStruct<ColorHSLA>("color_hsla",
		{
			CFIELD("h", ColorHSLA, h),
			CFIELD("s", ColorHSLA, s),
			CFIELD("l", ColorHSLA, l),
			CFIELD("a", ColorHSLA, a)
		},
		{
			CFASMETHOD("as_hsva", ColorHSLA, (ColorHSVA(*)(const ColorHSLA &))color_cast<ColorHSVA>),
			CFASMETHOD("as_rgba", ColorHSLA, (ColorRGBA(*)(const ColorHSLA &))color_cast<ColorRGBA>),
			CMETHOD("multiply", ColorHSLA, Multiply),
		});

	m_Bridge.BridgeStruct<ColorHSVA>("color_hsva",
		{
			CFIELD("h", ColorHSVA, h),
			CFIELD("s", ColorHSVA, s),
			CFIELD("v", ColorHSVA, v),
			CFIELD("a", ColorHSVA, a)
		},
		{
			CFASMETHOD("as_hsla", ColorHSVA, (ColorHSLA(*)(const ColorHSVA &))color_cast<ColorHSLA>),
			CFASMETHOD("as_rgba", ColorHSVA, (ColorRGBA(*)(const ColorHSVA &))color_cast<ColorRGBA>),
			CMETHOD("multiply", ColorHSVA, Multiply),
		});

	m_Bridge.BridgeStruct<ColorRGBA>("color_rgba",
		{
			CFIELD("r", ColorRGBA, r),
			CFIELD("g", ColorRGBA, g),
			CFIELD("b", ColorRGBA, b),
			CFIELD("a", ColorRGBA, a)
		},
		{
			CFASMETHOD("as_hsva", ColorRGBA, (ColorHSVA(*)(const ColorRGBA &))color_cast<ColorHSVA>),
			CFASMETHOD("as_hsla", ColorRGBA, (ColorHSLA(*)(const ColorRGBA &))color_cast<ColorHSLA>),
			CMETHOD("multiply", ColorRGBA, Multiply),
		});

	m_Bridge.BridgeFunction("test", test1);
	m_Bridge.BridgeFunction("test", test2);
	m_Bridge.BridgeFunction("test", test3);
	m_Bridge.BridgeFunction("test", test4);
}

void CLua::Init()
{
	if (Active())
		return;

	m_pLuaState = luaL_newstate();
	luaopen_base(m_pLuaState);
	luaopen_math(m_pLuaState);
	luaopen_utf8(m_pLuaState);
	luaopen_string(m_pLuaState);
	luaopen_table(m_pLuaState);

	m_Bridge.Setup(m_pLuaState);

	DoScript(R"(
print = ddnet.print;

local function view(obj, i)
	local offset = string.rep("  ", i);

	if type(obj) == "nil" then
		print(offset .. "[nil]");
	elseif type(obj) == "boolean" then
		if obj then
			print(offset .. "true");
		else
			print(offset .. "false");
		end
	elseif type(obj) == "number" then
		print(offset .. obj);
	elseif type(obj) == "string" then
		print(offset .. "'" .. obj .. "'");
	elseif type(obj) == "function" then
		print(offset .. "[function]");
	elseif type(obj) == "table" then
		print(offset .. "{");
		for k, v in pairs(obj) do
			print(offset .. "[key]:");
			view(k, i + 1);
			print(offset .. "[value]:");
			if v == obj then
				print(offset .. "  [self]");
			else
				view(v, i + 1);
			end
		end
		print(offset .. "}");
	end
end;

ddnet.view = function(obj) view(obj, 0) end;
	)");
}

void CLua::Close()
{
	if (!Active())
		return;

	lua_close(m_pLuaState);
	m_pLuaState = nullptr;
}

void CLua::OnRender(int LuaRenderType)
{
}

void CLua::OnRender(int LuaRenderType, CUIRect Box)
{
}

void CLua::LuaPrint(const std::string &Text)
{
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "lua", Text.c_str(), gs_LuaPrintColor);
}

/*
const char *CLua::ms_apEventLuaNames[] = {
	"on_state_change",
	"on_init",
	"on_shutdown",
	"on_reset",
	"on_window_resize",
	"on_refresh_skins",
	"on_render",
	"on_new_snapshot",
	"on_release",
	"on_map_load",
	"on_message",
	"on_input",
};

void CLua::InitLua()
{
	if(Active())
		CloseLua();

	m_CurrentEvent = LUA_EVENT_TYPE_NONE;

	m_pLuaState = luaL_newstate();
	luaL_openlibs(m_pLuaState);

#define ADD_INT(Name, Value, ...) \
	do \
	{ \
		lua_pushinteger(m_pLuaState, Value); \
		__VA_ARGS__ \
		lua_setfield(m_pLuaState, -2, Name); \
	} while(0)

#define ADD_STRING(Name, Value, ...) \
	do \
	{ \
		lua_pushstring(m_pLuaState, Value); \
		__VA_ARGS__ \
		lua_setfield(m_pLuaState, -2, Name); \
	} while(0)

#define ADD_BOOLEAN(Name, Value, ...) \
	do \
	{ \
		lua_pushboolean(m_pLuaState, Value); \
		__VA_ARGS__ \
		lua_setfield(m_pLuaState, -2, Name); \
	} while(0)

#define ADD_NIL(Name, ...) \
	do \
	{ \
		lua_pushnil(m_pLuaState); \
		__VA_ARGS__ \
		lua_setfield(m_pLuaState, -2, Name); \
	} while(0)

#define ADD_FUNC(Name, Func, ...) \
	do \
	{ \
		lua_pushlightuserdata(m_pLuaState, this); \
		lua_pushcclosure(m_pLuaState, Func, 1); \
		__VA_ARGS__ \
		lua_setfield(m_pLuaState, -2, Name); \
	} while(0)

#define ADD_FIELD(Name, ...) \
	do \
	{ \
		lua_newtable(m_pLuaState); \
		__VA_ARGS__ \
		lua_setfield(m_pLuaState, -2, Name); \
	} while(0)

	// print

	lua_pushlightuserdata(m_pLuaState, this);
	lua_pushcclosure(m_pLuaState, LuaPrint, 1);
	lua_setglobal(m_pLuaState, "print");

	// override lua dofile function

	lua_pushlightuserdata(m_pLuaState, this);
	lua_pushcclosure(m_pLuaState, LuaDoFile, 1);
	lua_setglobal(m_pLuaState, "dofile");

	
	// override file finder and reader

	lua_getglobal(m_pLuaState, "package");
	lua_newtable(m_pLuaState);
	lua_pushlightuserdata(m_pLuaState, this);
	lua_pushcclosure(m_pLuaState, LuaFindFile, 1); \
	lua_rawseti(m_pLuaState, -2, 1);
	lua_setfield(m_pLuaState, -2, "searchers");
	lua_pop(m_pLuaState, 1);

	// Metatables

	luaL_newmetatable(m_pLuaState, "UIRectMetatable");

	ADD_FUNC("new", LuaNewUIRect);

	ADD_FUNC("h_split_mid", [](lua_State *pLuaState) -> int { return BasicLuaUIRectSplitMid(pLuaState, &CUIRect::HSplitMid); });

	ADD_FUNC("h_split_top", [](lua_State *pLuaState) -> int { return BasicLuaUIRectSplitSide(pLuaState, &CUIRect::HSplitTop); });

	ADD_FUNC("h_split_bottom", [](lua_State *pLuaState) -> int { return BasicLuaUIRectSplitSide(pLuaState, &CUIRect::HSplitBottom); });

	ADD_FUNC("v_split_mid", [](lua_State *pLuaState) -> int { return BasicLuaUIRectSplitMid(pLuaState, &CUIRect::VSplitMid); });

	ADD_FUNC("v_split_left", [](lua_State *pLuaState) -> int { return BasicLuaUIRectSplitSide(pLuaState, &CUIRect::VSplitLeft); });

	ADD_FUNC("v_split_right", [](lua_State *pLuaState) -> int { return BasicLuaUIRectSplitSide(pLuaState, &CUIRect::VSplitRight); });

	ADD_FUNC("margin", LuaUIRectMargin);

	ADD_FUNC("v_margin", [](lua_State *pLuaState) -> int { return BasicLuaUIRectMargin(pLuaState, &CUIRect::VMargin); });

	ADD_FUNC("h_margin", [](lua_State *pLuaState) -> int { return BasicLuaUIRectMargin(pLuaState, &CUIRect::HMargin); });

	ADD_FUNC("draw", LuaUIRectDraw);

	ADD_FUNC("draw4", LuaUIRectDraw4);

	// TODO: maybe add Inside func

	lua_pushvalue(m_pLuaState, -1);
	lua_setfield(m_pLuaState, -2, "__index");

	lua_pop(m_pLuaState, 1);

	luaL_newmetatable(m_pLuaState, "ReadOnlyMetatable");

	lua_pushcfunction(m_pLuaState, [](lua_State *pLuaState) -> int 
		{
			return luaL_error(pLuaState, "attempt to modify a read-only variable");
		});
	lua_setfield(m_pLuaState, -2, "__newindex");

	lua_pushvalue(m_pLuaState, -1);
	lua_setfield(m_pLuaState, -2, "__index");

	lua_pop(m_pLuaState, 1);


#define MAKE_READONLY \
	luaL_getmetatable(m_pLuaState, "ReadOnlyMetatable"); \
	lua_setmetatable(m_pLuaState, -2);

	// neorace functions

	lua_newtable(m_pLuaState);

	ADD_FIELD("config",
		ADD_FUNC("set", LuaSetSetting);
		ADD_FUNC("get", LuaGetSetting);
		ADD_FUNC("get_info", LuaGetSettingInfo);
		MAKE_READONLY;
	);

	ADD_FIELD("console",
		ADD_FUNC("run", LuaConsoleRunCommand);
		MAKE_READONLY;
	);

	ADD_FIELD("game",
		ADD_FUNC("get_game_tick", LuaGetGameTick);
		ADD_FUNC("get_game_tick_speed", LuaGetGameTickSpeed);
		ADD_FUNC("get_client_data", LuaGetClientData);
		ADD_FUNC("get_client_stats", LuaGetClientStats);
		ADD_FUNC("get_client_team", LuaGetClientTeam);
		ADD_FUNC("get_my_id", LuaGetLocalId);
		ADD_FUNC("get_my_dummy_id", LuaGetLocalDummyId);
		ADD_FUNC("get_my_name", LuaGetLocalName);
		ADD_FUNC("get_my_dummy_name", LuaGetLocalDummyName);
		ADD_FUNC("is_dummy_controlled", LuaIsDummyControlled);
		ADD_FUNC("toggle_dummy", LuaToggleDummyControl);
		ADD_FUNC("update_my_info", LuaSendInfo);
		ADD_FUNC("update_my_dummy_info", LuaSendDummyInfo);
		ADD_FUNC("get_state", LuaGetState);
		ADD_FUNC("disconnect", LuaDisconnect);
		ADD_FUNC("disconnect_dummy", LuaDisconnectDummy);
		ADD_FUNC("connect", LuaConnect);
		ADD_FUNC("connect_dummy", LuaConnectDummy);
		ADD_FUNC("is_dummy_allowed", LuaIsDummyAllowed);
		ADD_FUNC("is_dummy_connected", LuaIsDummyConnected);
		ADD_FUNC("is_dummy_connecting", LuaIsDummyConnecting);
		ADD_FUNC("quit", LuaQuit);
		ADD_FUNC("restart", LuaRestart);
		ADD_FUNC("kill", LuaKill); // TODO: maybe move to console
		MAKE_READONLY;
	);
	/*	CServerInfo ServerInfo;
	Client()->GetServerInfo(&ServerInfo);*/
	/* if(ServerBrowser()->DDNetInfoAvailable())
	{
		// Initially add DDNet as favorite community and select its tab.
		// This must be delayed until the DDNet info is available.
		if(m_CreateDefaultFavoriteCommunities)
		{
			m_CreateDefaultFavoriteCommunities = false;
			if(ServerBrowser()->Community(IServerBrowser::COMMUNITY_DDNET) != nullptr)*//*


	ADD_FIELD("ui",
		ADD_FUNC("get_screen_rect", LuaGetScreenUIRect);
		ADD_FUNC("do_label", LuaUIDoLabel);
		ADD_FUNC("map_screen", LuaMapScreen);
		ADD_FUNC("pixel_size", LuaPixelSize);
		MAKE_READONLY;
	);

	ADD_FIELD("event",
		for(int i = 0; i < LUA_EVENT_TYPE_COUNT; i++)
			ADD_NIL(ms_apEventLuaNames[i]);
		MAKE_READONLY;
	);

	ADD_FIELD("constants",
		ADD_FIELD("key",
			for(int Key = KEY_FIRST; Key < KEY_LAST; Key++)
			{
				const char *pName = Input()->KeyName(Key);
				if(pName[0] == '&')
					continue;
				ADD_INT(pName, Key);
			} 
			MAKE_READONLY;
		);
		ADD_FIELD("state",
			ADD_INT("offline", IClient::STATE_OFFLINE);
			ADD_INT("connecting", IClient::STATE_CONNECTING);
			ADD_INT("loading", IClient::STATE_LOADING);
			ADD_INT("online", IClient::STATE_ONLINE);
			ADD_INT("demoplayback", IClient::STATE_DEMOPLAYBACK);
			ADD_INT("quitting", IClient::STATE_QUITTING);
			ADD_INT("restarting", IClient::STATE_RESTARTING);
			MAKE_READONLY;
		);
		ADD_FIELD("input_flag",
			ADD_INT("press", IInput::FLAG_PRESS);
			ADD_INT("release", IInput::FLAG_RELEASE);
			ADD_INT("text", IInput::FLAG_TEXT);
			MAKE_READONLY;
		);
		ADD_FIELD("corner", 
			ADD_INT("none", IGraphics::CORNER_NONE);
			ADD_INT("top_left", IGraphics::CORNER_TL);
			ADD_INT("top_right", IGraphics::CORNER_TR);
			ADD_INT("bottom_left", IGraphics::CORNER_BL);
			ADD_INT("bottom_right", IGraphics::CORNER_BR);
			ADD_INT("top", IGraphics::CORNER_T);
			ADD_INT("bottom", IGraphics::CORNER_B);
			ADD_INT("left", IGraphics::CORNER_L);
			ADD_INT("right", IGraphics::CORNER_R);
			ADD_INT("all", IGraphics::CORNER_ALL);
			MAKE_READONLY;
		);
		ADD_FIELD("text_align",
			ADD_INT("left", ETextAlignment::TEXTALIGN_LEFT);
			ADD_INT("center", ETextAlignment::TEXTALIGN_CENTER);
			ADD_INT("right", ETextAlignment::TEXTALIGN_RIGHT);
			ADD_INT("top", ETextAlignment::TEXTALIGN_TOP);
			ADD_INT("middle", ETextAlignment::TEXTALIGN_MIDDLE);
			ADD_INT("bottom", ETextAlignment::TEXTALIGN_BOTTOM);
			ADD_INT("top_left", ETextAlignment::TEXTALIGN_TL);
			ADD_INT("top_center", ETextAlignment::TEXTALIGN_TC);
			ADD_INT("top_right", ETextAlignment::TEXTALIGN_TR);
			ADD_INT("middle_left", ETextAlignment::TEXTALIGN_ML);
			ADD_INT("middle_center", ETextAlignment::TEXTALIGN_MC);
			ADD_INT("middle_right", ETextAlignment::TEXTALIGN_MR);
			ADD_INT("bottom_left", ETextAlignment::TEXTALIGN_BL);
			ADD_INT("bottom_center", ETextAlignment::TEXTALIGN_BC);
			ADD_INT("bottom_right", ETextAlignment::TEXTALIGN_BR);
			MAKE_READONLY;
		);
		ADD_FIELD("render",
			ADD_FIELD("layer",
				ADD_INT("first", LUA_RENDER_LAYER_FIRST);
				ADD_INT("above_background", LUA_RENDER_LAYER_ABOVE_BACKGROUND);
				ADD_INT("above_players", LUA_RENDER_LAYER_ABOVE_PLAYERS);
				ADD_INT("above_foreground", LUA_RENDER_LAYER_ABOVE_FOREGROUND);
				ADD_INT("under_hud", LUA_RENDER_LAYER_UNDER_HUD);
				ADD_INT("above_hud", LUA_RENDER_LAYER_ABOVE_HUD);
				ADD_INT("above_menus", LUA_RENDER_LAYER_ABOVE_MENUS);
				ADD_INT("last", LUA_RENDER_LAYER_LAST);
				MAKE_READONLY;
			);
			ADD_INT("menu", LUA_RENDER_MENU);
			MAKE_READONLY;
		);
		ADD_FIELD("message_type",
			    ADD_INT("sv_motd", NETMSGTYPE_SV_MOTD);
			    ADD_INT("sv_broadcast", NETMSGTYPE_SV_BROADCAST);
			    ADD_INT("sv_chat", NETMSGTYPE_SV_CHAT);
			    ADD_INT("sv_killmsg", NETMSGTYPE_SV_KILLMSG);
			    ADD_INT("sv_soundglobal", NETMSGTYPE_SV_SOUNDGLOBAL);
			    ADD_INT("sv_tuneparams", NETMSGTYPE_SV_TUNEPARAMS);
			    ADD_INT("unused", NETMSGTYPE_UNUSED);
			    ADD_INT("sv_readytoenter", NETMSGTYPE_SV_READYTOENTER);
			    ADD_INT("sv_weaponpickup", NETMSGTYPE_SV_WEAPONPICKUP);
			    ADD_INT("sv_emoticon", NETMSGTYPE_SV_EMOTICON);
			    ADD_INT("sv_voteclearoptions", NETMSGTYPE_SV_VOTECLEAROPTIONS);
			    ADD_INT("sv_voteoptionlistadd", NETMSGTYPE_SV_VOTEOPTIONLISTADD);
			    ADD_INT("sv_voteoptionadd", NETMSGTYPE_SV_VOTEOPTIONADD);
			    ADD_INT("sv_voteoptionremove", NETMSGTYPE_SV_VOTEOPTIONREMOVE);
			    ADD_INT("sv_voteset", NETMSGTYPE_SV_VOTESET);
			    ADD_INT("sv_votestatus", NETMSGTYPE_SV_VOTESTATUS);
			    //ADD_INT("cl_say", NETMSGTYPE_CL_SAY);
			    //ADD_INT("cl_setteam", NETMSGTYPE_CL_SETTEAM);
			    //ADD_INT("cl_setspectatormode", NETMSGTYPE_CL_SETSPECTATORMODE);
			    //ADD_INT("cl_startinfo", NETMSGTYPE_CL_STARTINFO);
			    //ADD_INT("cl_changeinfo", NETMSGTYPE_CL_CHANGEINFO);
			    //ADD_INT("cl_kill", NETMSGTYPE_CL_KILL);
			    //ADD_INT("cl_emoticon", NETMSGTYPE_CL_EMOTICON);
			    //ADD_INT("cl_vote", NETMSGTYPE_CL_VOTE);
			    //ADD_INT("cl_callvote", NETMSGTYPE_CL_CALLVOTE);
			    //ADD_INT("cl_isddnetlegacy", NETMSGTYPE_CL_ISDDNETLEGACY);
			    ADD_INT("sv_ddracetimelegacy", NETMSGTYPE_SV_DDRACETIMELEGACY);
			    ADD_INT("sv_recordlegacy", NETMSGTYPE_SV_RECORDLEGACY);
			    ADD_INT("unused2", NETMSGTYPE_UNUSED2);
			    ADD_INT("sv_teamsstatelegacy", NETMSGTYPE_SV_TEAMSSTATELEGACY);
			    //ADD_INT("cl_showotherslegacy", NETMSGTYPE_CL_SHOWOTHERSLEGACY);
			    ADD_INT("sv_myownmessage", NETMSGTYPE_SV_MYOWNMESSAGE);
			    //ADD_INT("cl_showdistance", NETMSGTYPE_CL_SHOWDISTANCE);
			    //ADD_INT("cl_showothers", NETMSGTYPE_CL_SHOWOTHERS);
			    ADD_INT("sv_teamsstate", NETMSGTYPE_SV_TEAMSSTATE);
			    ADD_INT("sv_ddracetime", NETMSGTYPE_SV_DDRACETIME);
			    ADD_INT("sv_record", NETMSGTYPE_SV_RECORD);
			    ADD_INT("sv_killmsgteam", NETMSGTYPE_SV_KILLMSGTEAM);
			    ADD_INT("sv_yourvote", NETMSGTYPE_SV_YOURVOTE);
			    ADD_INT("sv_racefinish", NETMSGTYPE_SV_RACEFINISH);
			    ADD_INT("sv_commandinfo", NETMSGTYPE_SV_COMMANDINFO);
			    ADD_INT("sv_commandinforemove", NETMSGTYPE_SV_COMMANDINFOREMOVE);
			    ADD_INT("sv_voteoptiongroupstart", NETMSGTYPE_SV_VOTEOPTIONGROUPSTART);
			    ADD_INT("sv_voteoptiongroupend", NETMSGTYPE_SV_VOTEOPTIONGROUPEND);
			    ADD_INT("sv_commandinfogroupstart", NETMSGTYPE_SV_COMMANDINFOGROUPSTART);
			    ADD_INT("sv_commandinfogroupend", NETMSGTYPE_SV_COMMANDINFOGROUPEND);
			    ADD_INT("sv_changeinfocooldown", NETMSGTYPE_SV_CHANGEINFOCOOLDOWN);
			    MAKE_READONLY;
		);
		ADD_FIELD("emoticon",
			ADD_INT("oop", EMOTICON_OOP);
			ADD_INT("exclamation", EMOTICON_EXCLAMATION);
			ADD_INT("hearts", EMOTICON_HEARTS);
			ADD_INT("drop", EMOTICON_DROP);
			ADD_INT("dotdot", EMOTICON_DOTDOT);
			ADD_INT("music", EMOTICON_MUSIC);
			ADD_INT("sorry", EMOTICON_SORRY);
			ADD_INT("ghost", EMOTICON_GHOST);
			ADD_INT("sushi", EMOTICON_SUSHI);
			ADD_INT("splattee", EMOTICON_SPLATTEE);
			ADD_INT("deviltee", EMOTICON_DEVILTEE);
			ADD_INT("zomg", EMOTICON_ZOMG);
			ADD_INT("zzz", EMOTICON_ZZZ);
			ADD_INT("wtf", EMOTICON_WTF);
			ADD_INT("eyes", EMOTICON_EYES);
			ADD_INT("question", EMOTICON_QUESTION);
			MAKE_READONLY;
		);
		ADD_FIELD("weapon",
			ADD_INT("hammer", WEAPON_HAMMER);
			ADD_INT("gun", WEAPON_GUN);
			ADD_INT("shotgun", WEAPON_SHOTGUN);
			ADD_INT("grenade", WEAPON_GRENADE);
			ADD_INT("laser", WEAPON_LASER);
			ADD_INT("ninja", WEAPON_NINJA);
			MAKE_READONLY;
		);
		ADD_INT("max_clients", MAX_CLIENTS);
		ADD_INT("max_player_clients", NUM_DUMMIES);
		ADD_FIELD("snapshot",
			ADD_INT("current", IClient::SNAP_CURRENT);
			ADD_INT("previous", IClient::SNAP_PREV);
		);
		MAKE_READONLY;
	);

	MAKE_READONLY;
	lua_setglobal(m_pLuaState, "neorace");

	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "lua", "Lua initialized", gs_LuaMessageColor);

#undef MAKE_READONLY
#undef ADD_INT
#undef ADD_FUNC
#undef ADD_EVENT_MENAGER
#undef ADD_STOPPABLE_EVENT_MENAGER
#undef ADD_FIELD
}

void CLua::CloseLua()
{
	if(Active())
	{
		lua_close(m_pLuaState);
		m_pLuaState = nullptr;
	}
}

CLua *CLua::GetSelf(lua_State *pLuaState)
{
	return (CLua *)lua_touserdata(pLuaState, lua_upvalueindex(1));
}

// TODO: Temporary solution (not for all functions)
#define DISABLE_IN_EVENT(...) \
do { \
if(pLua->m_CurrentEvent != LUA_EVENT_TYPE_NONE) \
{ \
	int Events[] = { __VA_ARGS__ }; \
	for (int i = 0; i < sizeof(Events) / sizeof(Events[0]); i++) \
	{ \
		if (pLua->m_CurrentEvent == Events[i]) \
		{ \
			luaL_error(pLuaState, "This function cannot be called in '%s' event", CLua::ms_apEventLuaNames[Events[i]]); \
		} \
	} \
} \
} while(0)

#define ENABLE_IN_EVENT(...) \
do { \
int Events[] = {__VA_ARGS__}; \
bool Allowed = false; \
for(int i = 0; i < sizeof(Events) / sizeof(Events[0]); i++) \
{ \
	if(pLua->m_CurrentEvent == Events[i]) \
	{ \
		Allowed = true; \
		break; \
	} \
} \
if (!Allowed) \
{ \
	luaL_error(pLuaState, "This function cannot be called in '%s' event", CLua::ms_apEventLuaNames[pLua->m_CurrentEvent]); \
} \
} while(0)

#define ENABLE_IN_CONNECTED_SERVER() \
do { \
if(pLua->Client()->State() != IClient::STATE_ONLINE) \
{ \
	luaL_error(pLuaState, "This function cannot be called when not connected to the server"); \
} \
} while(0)

void CLua::OnConsoleInit()
{
	Console()->Register("lua", "s[script]", CFGFLAG_CLIENT, ConLua, this, "Run lua script");
	Console()->Register("exec_lua", "s[file]", CFGFLAG_CLIENT, ConExecLua, this, "Run lua file");
}

const char *Reader(lua_State *pLuaState, void *pUserData, size_t *pSize)
{
	IOHANDLE *pFile = (IOHANDLE *)pUserData;

	if(!*pFile)
	{
		*pSize = 0;
		return nullptr;
	}

	char *pBuffer = io_read_all_str(*pFile);
	io_close(*pFile);
	*pFile = nullptr;

	if(pBuffer == nullptr)
	{
		return nullptr;
	}

	*pSize = str_length(pBuffer);
	return pBuffer;
}

int CLua::LuaLoadFile(lua_State* pLuaState)
{
	CLua *pLua = GetSelf(pLuaState);

	const char *pFilename = luaL_checkstring(pLuaState, -1);

	IOHANDLE File = pLua->Storage()->OpenFile(pFilename, IOFLAG_READ, IStorage::TYPE_ALL);

	if(File)
	{
		if(lua_load(pLuaState, Reader, &File, pFilename, "t") != LUA_OK)
		{
			return lua_error(pLuaState);
		}
	}
	else
		return luaL_error(pLuaState, "Cannot open file: %s", pFilename);

	return 1; // Return the chunk
}

int CLua::LuaFindFile(lua_State *pLuaState)
{
	CLua *pLua = GetSelf(pLuaState);

	const char *pFilename = luaL_checkstring(pLuaState, 1);

	char aPath[IO_MAX_PATH_LENGTH];

	if(!pLua->Storage()->FindFile(pFilename, "plugins", IStorage::TYPE_ALL, aPath, sizeof(aPath)))
	{
		if(pLua->Storage()->FileExists(pFilename, IStorage::TYPE_ALL))
		{
			str_copy(aPath, pFilename, sizeof(aPath));
		}
		else
		{
			lua_pushnil(pLuaState);
			lua_pushstring(pLuaState, "Cannot find file");
			return 2;
		}
	}

	lua_pushlightuserdata(pLuaState, pLua);
	lua_pushcclosure(pLuaState, LuaLoadFile, 1);
	lua_pushstring(pLuaState, aPath);
	return 2;
}

int CLua::DoString(const char *pString)
{
	if(!Active())
		InitLua();

	return luaL_dostring(m_pLuaState, pString);
}

int CLua::DoFile(const char *pFilename)
{
	if(!Active())
		InitLua();

	lua_pushlightuserdata(m_pLuaState, this);
	lua_pushcclosure(m_pLuaState, LuaFindFile, 1);
	lua_pushstring(m_pLuaState, pFilename);

	{
		auto result = lua_pcall(m_pLuaState, 1, 2, 0); // find file
		if(result != LUA_OK)
			return result;
	}

	if(lua_isnil(m_pLuaState, -2))
	{
		lua_insert(m_pLuaState, -2);
		lua_pop(m_pLuaState, 1);
		return LUA_ERRFILE;
	}

	{
		auto result = lua_pcall(m_pLuaState, 1, 1, 0); // load chunk
		if(result != LUA_OK)
			return result;
	}

	{
		
		auto result = lua_pcall(m_pLuaState, 0, LUA_MULTRET, 0); // rim chunk
		if(result != LUA_OK)
			return result;
	}

	return LUA_OK;
}

void CLua::ConLua(IConsole::IResult *pResult, void *pUserData)
{
	CLua *pLua = (CLua *)pUserData;

	const char *pScript = pResult->GetString(0);

	if(pLua->DoString(pScript) != LUA_OK)
	{
		pLua->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "lua", lua_tostring(pLua->m_pLuaState, -1), gs_LuaErrorColor);
		lua_pop(pLua->m_pLuaState, 1);
	}
}

void CLua::ConExecLua(IConsole::IResult *pResult, void *pUserData)
{
	CLua *pLua = (CLua *)pUserData;

	const char *pFilepath = pResult->GetString(0);

	if (pLua->DoFile(pFilepath) != LUA_OK)
	{
		pLua->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "lua", lua_tostring(pLua->m_pLuaState, -1), gs_LuaErrorColor);
		lua_pop(pLua->m_pLuaState, 1);
	}
}

bool CLua::RunOnEvent(CLua *pLua, lua_State *pLuaState, EEventType EventType, int ArgsCount, int ReturnCount)
{
	if(!pLua->Active())
		return false;

	dbg_assert(EventType > LUA_EVENT_TYPE_NONE && EventType < LUA_EVENT_TYPE_COUNT, "Invalid event type");

	lua_getglobal(pLuaState, "neorace");
	if(lua_isnil(pLuaState, -1))
	{
		lua_pop(pLuaState, 1 + ArgsCount);
		return false;
	}
	lua_getfield(pLuaState, -1, "event");
	if(lua_isnil(pLuaState, -1))
	{
		lua_pop(pLuaState, 2 + ArgsCount);
		return false;
	}
	lua_getfield(pLuaState, -1, CLua::ms_apEventLuaNames[EventType]);
	if(lua_isnil(pLuaState, -1))
	{
		lua_pop(pLuaState, 3 + ArgsCount);
		return false;
	}

	// copy arguments to the top
	for (int i = 0; i < ArgsCount; i++)
	{
		lua_pushvalue(pLuaState, -3 - ArgsCount);
	}

	pLua->m_CurrentEvent = EventType;

	if (lua_pcall(pLuaState, ArgsCount, ReturnCount, 0) != LUA_OK)
	{
		pLua->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "lua", lua_tostring(pLuaState, -1), gs_LuaErrorColor);
		lua_pop(pLuaState, 1 + 2 + ArgsCount);
		return false;
	}

	pLua->m_CurrentEvent = LUA_EVENT_TYPE_NONE;

	// move return values before arguments
	for (int i = 0; i < ReturnCount; i++)
	{
		lua_insert(pLuaState, -ReturnCount - 2 - ArgsCount);
	}

	// remove neorace, event, on_event and arguments
	lua_pop(pLuaState, 2 + ArgsCount);

	return true;
}

void CLua::OnStateChange(int NewState, int OldState)
{
	if(!Active())
		return;

	lua_pushinteger(m_pLuaState, NewState);
	lua_pushinteger(m_pLuaState, OldState);

	RunOnEvent(this, m_pLuaState, LUA_EVENT_TYPE_ON_STATE_CHANGE, 2);
}

void CLua::OnInit()
{
	std::string_view InitFiles(Config()->m_ClLuaInitFiles);

	if(InitFiles.empty())
		return;

	for(size_t Start = 0, End = 0; End != std::string_view::npos; Start = End + 1)
	{
		End = InitFiles.find(';', Start);
		std::string_view Filename = InitFiles.substr(Start, End - Start);
		char aFilename[IO_MAX_PATH_LENGTH];
		str_copy(aFilename, Filename.data(), minimum(sizeof(aFilename), Filename.size() + 1));
		aFilename[minimum(sizeof(aFilename) - 1, Filename.size() + 1)] = '\0';
		DoFile(aFilename);
	}

	RunOnEvent(this, m_pLuaState, LUA_EVENT_TYPE_ON_INIT);
}

void CLua::OnShutdown()
{
	RunOnEvent(this, m_pLuaState, LUA_EVENT_TYPE_ON_SHUTDOWN);

	if(Active())
		CloseLua();
}

void CLua::OnReset()
{
	RunOnEvent(this, m_pLuaState, LUA_EVENT_TYPE_ON_RESET);
}

void CLua::OnWindowResize()
{
	RunOnEvent(this, m_pLuaState, LUA_EVENT_TYPE_ON_WINDOW_RESIZE);
}

void CLua::OnRefreshSkins()
{
	RunOnEvent(this, m_pLuaState, LUA_EVENT_TYPE_ON_REFRESH_SKINS);
}

void CLua::OnNewSnapshot()
{
	RunOnEvent(this, m_pLuaState, LUA_EVENT_TYPE_ON_NEW_SNAPSHOT);
}

void CLua::OnRelease()
{
	RunOnEvent(this, m_pLuaState, LUA_EVENT_TYPE_ON_RELEASE);
}

void CLua::OnMapLoad()
{
	RunOnEvent(this, m_pLuaState, LUA_EVENT_TYPE_ON_MAP_LOAD);
}

void CLua::OnMessage(int Msg, void* pRawMsg)
{
	if(!m_pLuaState)
		return;

	lua_newtable(m_pLuaState);

	lua_pushinteger(m_pLuaState, Msg);
	lua_setfield(m_pLuaState, -2, "type");

	switch(Msg)
	{
	case NETMSGTYPE_SV_MOTD:
	{
		CNetMsg_Sv_Motd *pMsg = (CNetMsg_Sv_Motd *)pRawMsg;

		lua_pushstring(m_pLuaState, pMsg->m_pMessage);
		lua_setfield(m_pLuaState, -2, "message");

		break;
	}
	case NETMSGTYPE_SV_BROADCAST:
	{
		CNetMsg_Sv_Broadcast *pMsg = (CNetMsg_Sv_Broadcast *)pRawMsg;

		lua_pushstring(m_pLuaState, pMsg->m_pMessage);
		lua_setfield(m_pLuaState, -2, "message");

		break;
	}
	case NETMSGTYPE_SV_CHAT:
	{
		CNetMsg_Sv_Chat *pMsg = (CNetMsg_Sv_Chat *)pRawMsg;

		lua_pushinteger(m_pLuaState, pMsg->m_Team);
		lua_setfield(m_pLuaState, -2, "team");

		lua_pushinteger(m_pLuaState, pMsg->m_ClientId);
		lua_setfield(m_pLuaState, -2, "client_id");

		lua_pushstring(m_pLuaState, pMsg->m_pMessage);
		lua_setfield(m_pLuaState, -2, "message");

		break;
	}
	case NETMSGTYPE_SV_KILLMSG:
	{
		CNetMsg_Sv_KillMsg *pMsg = (CNetMsg_Sv_KillMsg *)pRawMsg;

		lua_pushinteger(m_pLuaState, pMsg->m_Killer);
		lua_setfield(m_pLuaState, -2, "killer");

		lua_pushinteger(m_pLuaState, pMsg->m_Victim);
		lua_setfield(m_pLuaState, -2, "victim");

		lua_pushinteger(m_pLuaState, pMsg->m_Weapon);
		lua_setfield(m_pLuaState, -2, "weapon");

		lua_pushinteger(m_pLuaState, pMsg->m_ModeSpecial);
		lua_setfield(m_pLuaState, -2, "mode_special");

		break;
	}
	case NETMSGTYPE_SV_SOUNDGLOBAL:
	{
		CNetMsg_Sv_SoundGlobal *pMsg = (CNetMsg_Sv_SoundGlobal *)pRawMsg;

		lua_pushinteger(m_pLuaState, pMsg->m_SoundId);
		lua_setfield(m_pLuaState, -2, "sound_id");

		break;
	}
	case NETMSGTYPE_SV_TUNEPARAMS:
	{
		CNetMsg_Sv_TuneParams *pMsg = (CNetMsg_Sv_TuneParams *)pRawMsg;

		break;
	}
	case NETMSGTYPE_UNUSED:
	{
		CNetMsg_Unused *pMsg = (CNetMsg_Unused *)pRawMsg;

		break;
	}
	case NETMSGTYPE_SV_READYTOENTER:
	{
		CNetMsg_Sv_ReadyToEnter *pMsg = (CNetMsg_Sv_ReadyToEnter *)pRawMsg;

		break;
	}
	case NETMSGTYPE_SV_WEAPONPICKUP:
	{
		CNetMsg_Sv_WeaponPickup *pMsg = (CNetMsg_Sv_WeaponPickup *)pRawMsg;

		lua_pushinteger(m_pLuaState, pMsg->m_Weapon);
		lua_setfield(m_pLuaState, -2, "weapon");

		break;
	}
	case NETMSGTYPE_SV_EMOTICON:
	{
		CNetMsg_Sv_Emoticon *pMsg = (CNetMsg_Sv_Emoticon *)pRawMsg;

		lua_pushinteger(m_pLuaState, pMsg->m_ClientId);
		lua_setfield(m_pLuaState, -2, "client_id");

		lua_pushinteger(m_pLuaState, pMsg->m_Emoticon);
		lua_setfield(m_pLuaState, -2, "emoticon");

		break;
	}
	case NETMSGTYPE_SV_VOTECLEAROPTIONS:
	{
		CNetMsg_Sv_VoteClearOptions *pMsg = (CNetMsg_Sv_VoteClearOptions *)pRawMsg;

		break;
	}
	case NETMSGTYPE_SV_VOTEOPTIONLISTADD:
	{
		CNetMsg_Sv_VoteOptionListAdd *pMsg = (CNetMsg_Sv_VoteOptionListAdd *)pRawMsg;

		lua_pushinteger(m_pLuaState, pMsg->m_NumOptions);
		lua_setfield(m_pLuaState, -2, "num_options");

	#define ADD_DESCRIPTION(i) \
		if(pMsg->m_NumOptions > i) \
		{ \
			lua_pushstring(m_pLuaState, pMsg->m_pDescription##i); \
			lua_setfield(m_pLuaState, -2, "description"#i); \
		} \

		ADD_DESCRIPTION(0);
		ADD_DESCRIPTION(1);
		ADD_DESCRIPTION(2);
		ADD_DESCRIPTION(3);
		ADD_DESCRIPTION(4);
		ADD_DESCRIPTION(5);
		ADD_DESCRIPTION(6);
		ADD_DESCRIPTION(7);
		ADD_DESCRIPTION(8);
		ADD_DESCRIPTION(9);
		ADD_DESCRIPTION(10);
		ADD_DESCRIPTION(11);
		ADD_DESCRIPTION(12);
		ADD_DESCRIPTION(13);
		ADD_DESCRIPTION(14);

	#undef ADD_DESCRIPTION

		break;
	}
	case NETMSGTYPE_SV_VOTEOPTIONADD:
	{
		CNetMsg_Sv_VoteOptionAdd *pMsg = (CNetMsg_Sv_VoteOptionAdd *)pRawMsg;

		lua_pushstring(m_pLuaState, pMsg->m_pDescription);
		lua_setfield(m_pLuaState, -2, "description");

		break;
	}
	case NETMSGTYPE_SV_VOTEOPTIONREMOVE:
	{
		CNetMsg_Sv_VoteOptionRemove *pMsg = (CNetMsg_Sv_VoteOptionRemove *)pRawMsg;

		lua_pushstring(m_pLuaState, pMsg->m_pDescription);
		lua_setfield(m_pLuaState, -2, "description");

		break;
	}
	case NETMSGTYPE_SV_VOTESET:
	{
		CNetMsg_Sv_VoteSet *pMsg = (CNetMsg_Sv_VoteSet *)pRawMsg;

		lua_pushinteger(m_pLuaState, pMsg->m_Timeout);
		lua_setfield(m_pLuaState, -2, "timeout");

		lua_pushstring(m_pLuaState, pMsg->m_pDescription);
		lua_setfield(m_pLuaState, -2, "description");

		lua_pushstring(m_pLuaState, pMsg->m_pReason);
		lua_setfield(m_pLuaState, -2, "reason");

		break;
	}
	case NETMSGTYPE_SV_VOTESTATUS:
	{
		CNetMsg_Sv_VoteStatus *pMsg = (CNetMsg_Sv_VoteStatus *)pRawMsg;

		lua_pushinteger(m_pLuaState, pMsg->m_Yes);
		lua_setfield(m_pLuaState, -2, "yes");

		lua_pushinteger(m_pLuaState, pMsg->m_No);
		lua_setfield(m_pLuaState, -2, "no");

		lua_pushinteger(m_pLuaState, pMsg->m_Pass);
		lua_setfield(m_pLuaState, -2, "pass");

		lua_pushinteger(m_pLuaState, pMsg->m_Total);
		lua_setfield(m_pLuaState, -2, "total");

		break;
	}
	case NETMSGTYPE_CL_SAY:
	case NETMSGTYPE_CL_SETTEAM:
	case NETMSGTYPE_CL_SETSPECTATORMODE:
	case NETMSGTYPE_CL_STARTINFO:
	case NETMSGTYPE_CL_CHANGEINFO:
	case NETMSGTYPE_CL_KILL:
	case NETMSGTYPE_CL_EMOTICON:
	case NETMSGTYPE_CL_VOTE:
	case NETMSGTYPE_CL_CALLVOTE:
	case NETMSGTYPE_CL_ISDDNETLEGACY:
	{
		dbg_assert(false, "unexpected message type");
		break;
	}
	case NETMSGTYPE_SV_DDRACETIMELEGACY:
	{
		CNetMsg_Sv_DDRaceTimeLegacy *pMsg = (CNetMsg_Sv_DDRaceTimeLegacy *)pRawMsg;

		lua_pushinteger(m_pLuaState, pMsg->m_Time);
		lua_setfield(m_pLuaState, -2, "time");

		lua_pushinteger(m_pLuaState, pMsg->m_Check);
		lua_setfield(m_pLuaState, -2, "check");

		lua_pushinteger(m_pLuaState, pMsg->m_Finish);
		lua_setfield(m_pLuaState, -2, "finish");

		break;
	}
	case NETMSGTYPE_SV_RECORDLEGACY:
	{
		CNetMsg_Sv_RecordLegacy *pMsg = (CNetMsg_Sv_RecordLegacy *)pRawMsg;

		lua_pushinteger(m_pLuaState, pMsg->m_ServerTimeBest);
		lua_setfield(m_pLuaState, -2, "server_time_best");

		lua_pushinteger(m_pLuaState, pMsg->m_PlayerTimeBest);
		lua_setfield(m_pLuaState, -2, "player_time_best");

		break;
	}
	case NETMSGTYPE_UNUSED2:
	{
		CNetMsg_Unused2 *pMsg = (CNetMsg_Unused2 *)pRawMsg;

		break;
	}
	case NETMSGTYPE_SV_TEAMSSTATELEGACY:
	{
		CNetMsg_Sv_TeamsStateLegacy *pMsg = (CNetMsg_Sv_TeamsStateLegacy *)pRawMsg;

		break;
	}
	case NETMSGTYPE_CL_SHOWOTHERSLEGACY:
	{
		dbg_assert(false, "unexpected message type");
		break;
	}

	case NETMSGTYPE_SV_MYOWNMESSAGE:
	{
		CNetMsg_Sv_MyOwnMessage *pMsg = (CNetMsg_Sv_MyOwnMessage *)pRawMsg;

		lua_pushinteger(m_pLuaState, pMsg->m_Test);
		lua_setfield(m_pLuaState, -2, "test");

		break;
	}
	case NETMSGTYPE_CL_SHOWDISTANCE:
	case NETMSGTYPE_CL_SHOWOTHERS:
	{
		dbg_assert(false, "unexpected message type");
		break;
	}
	case NETMSGTYPE_SV_TEAMSSTATE:
	{
		CNetMsg_Sv_TeamsState *pMsg = (CNetMsg_Sv_TeamsState *)pRawMsg;

		break;
	}
	case NETMSGTYPE_SV_DDRACETIME:
	{
		CNetMsg_Sv_DDRaceTime *pMsg = (CNetMsg_Sv_DDRaceTime *)pRawMsg;

		lua_pushinteger(m_pLuaState, pMsg->m_Time);
		lua_setfield(m_pLuaState, -2, "time");

		lua_pushinteger(m_pLuaState, pMsg->m_Check);
		lua_setfield(m_pLuaState, -2, "check");

		lua_pushinteger(m_pLuaState, pMsg->m_Finish);
		lua_setfield(m_pLuaState, -2, "finish");

		break;
	}
	case NETMSGTYPE_SV_RECORD:
	{
		CNetMsg_Sv_Record *pMsg = (CNetMsg_Sv_Record *)pRawMsg;

		lua_pushinteger(m_pLuaState, pMsg->m_ServerTimeBest);
		lua_setfield(m_pLuaState, -2, "server_time_best");

		lua_pushinteger(m_pLuaState, pMsg->m_PlayerTimeBest);
		lua_setfield(m_pLuaState, -2, "player_time_best");

		break;
	}
	case NETMSGTYPE_SV_KILLMSGTEAM:
	{
		CNetMsg_Sv_KillMsgTeam *pMsg = (CNetMsg_Sv_KillMsgTeam *)pRawMsg;

		lua_pushinteger(m_pLuaState, pMsg->m_Team);
		lua_setfield(m_pLuaState, -2, "team");

		lua_pushinteger(m_pLuaState, pMsg->m_First);
		lua_setfield(m_pLuaState, -2, "first");

		break;
	}
	case NETMSGTYPE_SV_YOURVOTE:
	{
		CNetMsg_Sv_YourVote *pMsg = (CNetMsg_Sv_YourVote *)pRawMsg;

		lua_pushinteger(m_pLuaState, pMsg->m_Voted);
		lua_setfield(m_pLuaState, -2, "voted");

		break;
	}
	case NETMSGTYPE_SV_RACEFINISH:
	{
		CNetMsg_Sv_RaceFinish *pMsg = (CNetMsg_Sv_RaceFinish *)pRawMsg;

		lua_pushinteger(m_pLuaState, pMsg->m_ClientId);
		lua_setfield(m_pLuaState, -2, "client_id");

		lua_pushinteger(m_pLuaState, pMsg->m_Time);
		lua_setfield(m_pLuaState, -2, "time");

		lua_pushinteger(m_pLuaState, pMsg->m_Diff);
		lua_setfield(m_pLuaState, -2, "diff");

		lua_pushinteger(m_pLuaState, pMsg->m_RecordPersonal);
		lua_setfield(m_pLuaState, -2, "record_personal");

		lua_pushinteger(m_pLuaState, pMsg->m_RecordServer);
		lua_setfield(m_pLuaState, -2, "record_server");

		break;
	}
	case NETMSGTYPE_SV_COMMANDINFO:
	{
		CNetMsg_Sv_CommandInfo *pMsg = (CNetMsg_Sv_CommandInfo *)pRawMsg;

		lua_pushstring(m_pLuaState, pMsg->m_pName);
		lua_setfield(m_pLuaState, -2, "name");

		lua_pushstring(m_pLuaState, pMsg->m_pArgsFormat);
		lua_setfield(m_pLuaState, -2, "args_format");

		lua_pushstring(m_pLuaState, pMsg->m_pHelpText);
		lua_setfield(m_pLuaState, -2, "help_text");

		break;
	}
	case NETMSGTYPE_SV_COMMANDINFOREMOVE:
	{
		CNetMsg_Sv_CommandInfoRemove *pMsg = (CNetMsg_Sv_CommandInfoRemove *)pRawMsg;

		lua_pushstring(m_pLuaState, pMsg->m_pName);
		lua_setfield(m_pLuaState, -2, "name");

		break;
	}
	case NETMSGTYPE_SV_VOTEOPTIONGROUPSTART:
	{
		CNetMsg_Sv_VoteOptionGroupStart *pMsg = (CNetMsg_Sv_VoteOptionGroupStart *)pRawMsg;

		break;
	}
	case NETMSGTYPE_SV_VOTEOPTIONGROUPEND:
	{
		CNetMsg_Sv_VoteOptionGroupEnd *pMsg = (CNetMsg_Sv_VoteOptionGroupEnd *)pRawMsg;

		break;
	}
	case NETMSGTYPE_SV_COMMANDINFOGROUPSTART:
	{
		CNetMsg_Sv_CommandInfoGroupStart *pMsg = (CNetMsg_Sv_CommandInfoGroupStart *)pRawMsg;

		break;
	}
	case NETMSGTYPE_SV_COMMANDINFOGROUPEND:
	{
		CNetMsg_Sv_CommandInfoGroupEnd *pMsg = (CNetMsg_Sv_CommandInfoGroupEnd *)pRawMsg;

		break;
	}
	case NETMSGTYPE_SV_CHANGEINFOCOOLDOWN:
	{
		CNetMsg_Sv_ChangeInfoCooldown *pMsg = (CNetMsg_Sv_ChangeInfoCooldown *)pRawMsg;

		lua_pushinteger(m_pLuaState, pMsg->m_WaitUntil);
		lua_setfield(m_pLuaState, -2, "wait_until");

		break;
	}

	default: break;
	}

	RunOnEvent(this, m_pLuaState, LUA_EVENT_TYPE_ON_MESSAGE, 1);
}

void CLua::OnSendMessage(int Msg, void* pRawMsg)
{
	OnMessage(Msg, pRawMsg);
}

bool CLua::OnInput(const IInput::CEvent& Event)
{
	if(!m_pLuaState)
		return false;

	lua_newtable(m_pLuaState);

	lua_pushinteger(m_pLuaState, Event.m_Flags);
	lua_setfield(m_pLuaState, -2, "flags");

	lua_pushinteger(m_pLuaState, Event.m_Key);
	lua_setfield(m_pLuaState, -2, "key");

	lua_pushinteger(m_pLuaState, Event.m_InputCount);
	lua_setfield(m_pLuaState, -2, "input_count");

	lua_pushstring(m_pLuaState, Event.m_aText);
	lua_setfield(m_pLuaState, -2, "text");

	if(!RunOnEvent(this, m_pLuaState, LUA_EVENT_TYPE_ON_INPUT, 1, 1))
		return false;

	bool Result = lua_toboolean(m_pLuaState, -1);
	lua_pop(m_pLuaState, 1);
	return Result;
}


int CLua::CLuaRenderLayer::Index()
{
	return this - m_pClient->m_Lua.m_aRenderLayers;
}

void CLua::CLuaRenderLayer::OnRender()
{
	m_pClient->m_Lua.OnRenderLayer(Index());
}

void CLua::OnRenderLayer(int Layer, std::optional<CUIRect> Box)
{
	if(!m_pLuaState)
		return;

	lua_pushinteger(m_pLuaState, Layer);

	if(Box)
		PushUIRect(m_pLuaState, Box.value());

	RunOnEvent(this, m_pLuaState, LUA_EVENT_TYPE_ON_RENDER, Box ? 2 : 1);
}

void CLua::RenderMenu(CUIRect MenuView)
{
	if(!m_pLuaState)
		return;

	OnRenderLayer(LUA_RENDER_MENU, MenuView);
}

template <int Depth, bool TableMode = false>
int GetPrintStr(lua_State* pLuaState)
{
	int ArgsCount = lua_gettop(pLuaState);

	std::stringstream Str;

	for (int i = 1; i <= ArgsCount; i++)
	{
		if(lua_isnumber(pLuaState, i))
		{
			Str << lua_tonumber(pLuaState, i);
		}
		else if(lua_isstring(pLuaState, i))
		{
			Str << '"' << lua_tostring(pLuaState, i) << '"';
		}
		else if(lua_isboolean(pLuaState, i))
		{
			Str << (lua_toboolean(pLuaState, i) ? "true" : "false");
		}
		else if(lua_istable(pLuaState, i))
		{
			if constexpr(Depth == 0)
			{
				lua_pushnil(pLuaState);
				bool HaveAnyValues = lua_next(pLuaState, i) != 0;

				if (HaveAnyValues)
				{
					lua_pop(pLuaState, 2);
					Str << "{...}";
				}
				else
				{
					Str << "{}";
				}
			}
			else
			{
				Str << '{';
				lua_pushcclosure(pLuaState, GetPrintStr<Depth - 1, true>, 0);

				int Size = 0;
				bool Overflowed = false;
				lua_pushnil(pLuaState);
				while(lua_next(pLuaState, i) != 0)
				{
					if(Size > 25)
					{
						lua_pop(pLuaState, 2);
						Overflowed = true;
						break;
					}
					Size += 1;
					lua_pushvalue(pLuaState, -2);
				}

				lua_call(pLuaState, Size * 2, 1);
				Str << lua_tostring(pLuaState, -1);
				lua_pop(pLuaState, 1);

				if (Overflowed)
				{
					Str << ", ...";
				}

				Str << '}';
			}
		}
		else
		{
			Str << lua_typename(pLuaState, lua_type(pLuaState, i));
		}

		if constexpr (TableMode)
		{
			if(i % 2)
			{
				Str << " = ";
			}
			else if (i < ArgsCount)
			{
				Str << ", ";
			}
		}
		else if(i < ArgsCount)
		{
			Str << " ";
		}
	}

	lua_pushstring(pLuaState, Str.str().c_str());
	return 1;
}

template <>
int GetPrintStr<-1, true>(lua_State* pLuaState)
{
	dbg_assert(false, "this should never be called");
	return 0;
}

int CLua::LuaPrint(lua_State* pLuaState)
{
	CLua *pLua = GetSelf(pLuaState);

	int ArgsCount = lua_gettop(pLuaState);

	lua_pushcfunction(pLuaState, GetPrintStr<2>);
	lua_insert(pLuaState, 1);
	lua_call(pLuaState, ArgsCount, 1);

	pLua->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "lua", lua_tostring(pLuaState, -1), gs_LuaPrintColor);

	lua_pop(pLuaState, 1);

	return 0;
}

int CLua::LuaDoFile(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 1)
		return luaL_error(pLuaState, "expected 1 arguments");

	CLua *pLua = GetSelf(pLuaState);

	const char *pFilename = luaL_checkstring(pLuaState, 1);

	lua_pop(pLuaState, 1);

	if (pLua->DoFile(pFilename) != LUA_OK)
	{
		return lua_error(pLuaState);
	}

	return lua_gettop(pLuaState);
}

int CLua::LuaConsoleRunCommand(lua_State *pLuaState)
{
	if(lua_gettop(pLuaState) != 1)
		return luaL_error(pLuaState, "expected 1 arguments");

	CLua *pLua = GetSelf(pLuaState);

	const char *pCommand = luaL_checkstring(pLuaState, 1);

	pLua->Console()->ExecuteLine(pCommand);

	return 0;
}

static std::variant<const SConfigVariable *, int> get_config_variable(IConfigManager *pConfigMenager, lua_State *pLuaState, const char *pSetting)
{
	std::vector<const SConfigVariable *> PossibleSettings;
	pConfigMenager->PossibleConfigVariables(
		pSetting, CFGFLAG_CLIENT,
		[](const SConfigVariable *pVariable, void *pUserData) {
			((std::vector<const SConfigVariable *> *)pUserData)->push_back(pVariable);
		},
		&PossibleSettings);

	const SConfigVariable *pConfigVariable = nullptr;

	if(PossibleSettings.size() == 1)
	{
		pConfigVariable = PossibleSettings[0];
	}
	else
		for(const SConfigVariable *pPossibleSetting : PossibleSettings)
		{
			if(str_comp_nocase_num(pSetting, pPossibleSetting->m_pScriptName, maximum(str_length(pSetting), str_length(pPossibleSetting->m_pScriptName))) == 0)
			{
				pConfigVariable = pPossibleSetting;
				break;
			}
		}

	if(!pConfigVariable)
	{
		if(PossibleSettings.size() == 0)
			return luaL_error(pLuaState, "'%s' is not a valid setting", pSetting);

		std::stringstream Error;
		Error << "'" << pSetting << "' is ambiguous, possible settings are: ";

		for(size_t i = 0; i < PossibleSettings.size(); i++)
		{
			Error << "'" << PossibleSettings[i]->m_pScriptName << "'";
			if(i + 1 < PossibleSettings.size())
				Error << ", ";
		}

		return luaL_error(pLuaState, Error.str().c_str());
	}

	return pConfigVariable;
}

int CLua::LuaSetSetting(lua_State *pLuaState)
{
	if (lua_gettop(pLuaState) != 2)
		return luaL_error(pLuaState, "expected 2 arguments");

	CLua *pLua = GetSelf(pLuaState);

	const char *pSetting = luaL_checkstring(pLuaState, 1);

	auto result = get_config_variable(pLua->Kernel()->RequestInterface<IConfigManager>(), pLuaState, pSetting);

	if(std::holds_alternative<int>(result))
		return std::get<int>(result);

	const SConfigVariable *pConfigVariable = std::get<const SConfigVariable *>(result);

	if(pConfigVariable->m_Type == SConfigVariable::EVariableType::VAR_INT)
	{
		const auto *pIntConfigVariable = (const SIntConfigVariable *)pConfigVariable;

		int Value = luaL_checkinteger(pLuaState, 2);

		// do clamping
		if(pIntConfigVariable->m_Min != pIntConfigVariable->m_Max)
		{
			if(Value < pIntConfigVariable->m_Min)
				Value = pIntConfigVariable->m_Min;
			if(pIntConfigVariable->m_Max != 0 && Value > pIntConfigVariable->m_Max)
				Value = pIntConfigVariable->m_Max;
		}

		*pIntConfigVariable->m_pVariable = Value;
	}
	else if(pConfigVariable->m_Type == SConfigVariable::EVariableType::VAR_STRING)
	{
		const auto *pStringConfigVariable = (const SStringConfigVariable *)pConfigVariable;

		const char *pValue = luaL_checkstring(pLuaState, 2);

		str_copy(pStringConfigVariable->m_pStr, pValue, pStringConfigVariable->m_MaxSize);
	}
	else if(pConfigVariable->m_Type == SConfigVariable::EVariableType::VAR_COLOR)
	{
		const auto *pColorConfigVariable = (const SColorConfigVariable *)pConfigVariable;

		auto Color = CheckColor(pLuaState, 2);
		const unsigned Value = Color.Pack(pColorConfigVariable->m_Light ? 0.5f : 0.0f, pColorConfigVariable->m_Alpha);

		*pColorConfigVariable->m_pVariable = Value;
	}
	else
	{
		return luaL_error(pLuaState, "type of '%s' is not supported in lua", pSetting);
	}

	return 0;
}

int CLua::LuaGetSetting(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 1)
		return luaL_error(pLuaState, "expected 1 arguments");

	CLua *pLua = GetSelf(pLuaState);

	const char *pSetting = luaL_checkstring(pLuaState, 1);

	auto result = get_config_variable(pLua->Kernel()->RequestInterface<IConfigManager>(), pLuaState, pSetting);

	if(std::holds_alternative<int>(result))
		return std::get<int>(result);

	const SConfigVariable *pConfigVariable = std::get<const SConfigVariable *>(result);

	if (pConfigVariable->m_Type == SConfigVariable::EVariableType::VAR_INT)
	{
		const auto *pIntConfigVariable = (const SIntConfigVariable *)pConfigVariable;

		lua_pushinteger(pLuaState, *(pIntConfigVariable->m_pVariable));
	}
	else if (pConfigVariable->m_Type == SConfigVariable::EVariableType::VAR_STRING)
	{
		const auto *pStringConfigVariable = (const SStringConfigVariable *)pConfigVariable;

		lua_pushstring(pLuaState, pStringConfigVariable->m_pStr);
	}
	else if (pConfigVariable->m_Type == SConfigVariable::EVariableType::VAR_COLOR)
	{
		const auto *pColorConfigVariable = (const SColorConfigVariable *)pConfigVariable;

		auto Color = ColorHSLA(*pColorConfigVariable->m_pVariable, true);
		if(pColorConfigVariable->m_Light)
			Color = Color.UnclampLighting();

		PushColor(pLuaState, Color, pColorConfigVariable->m_Alpha);
	}
	else
	{
		lua_pushnil(pLuaState);
	}

	return 1;
}

int CLua::LuaGetSettingInfo(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 1)
		return luaL_error(pLuaState, "expected 1 arguments");

	CLua *pLua = GetSelf(pLuaState);

	const char *pSetting = luaL_checkstring(pLuaState, 1);

	auto result = get_config_variable(pLua->Kernel()->RequestInterface<IConfigManager>(), pLuaState, pSetting);

	if(std::holds_alternative<int>(result))
		return std::get<int>(result);

	const SConfigVariable *pConfigVariable = std::get<const SConfigVariable *>(result);

	lua_newtable(pLuaState);

	lua_pushstring(pLuaState, pConfigVariable->m_pScriptName);
	lua_setfield(pLuaState, -2, "name");

	lua_pushinteger(pLuaState, pConfigVariable->m_Type);
	lua_setfield(pLuaState, -2, "type");

	lua_pushstring(pLuaState, pConfigVariable->m_pHelp);
	lua_setfield(pLuaState, -2, "help");

	if(pConfigVariable->m_Type == SConfigVariable::EVariableType::VAR_INT)
	{
		const auto *pIntConfigVariable = (const SIntConfigVariable *)pConfigVariable;

		lua_pushinteger(pLuaState, pIntConfigVariable->m_Default);
		lua_setfield(pLuaState, -2, "default");

		lua_pushinteger(pLuaState, pIntConfigVariable->m_Min);
		lua_setfield(pLuaState, -2, "min");

		lua_pushinteger(pLuaState, pIntConfigVariable->m_Max);
		lua_setfield(pLuaState, -2, "max");
	}
	else if(pConfigVariable->m_Type == SConfigVariable::EVariableType::VAR_STRING)
	{
		const auto *pStringConfigVariable = (const SStringConfigVariable *)pConfigVariable;

		lua_pushstring(pLuaState, pStringConfigVariable->m_pDefault);
		lua_setfield(pLuaState, -2, "default");

		lua_pushinteger(pLuaState, pStringConfigVariable->m_MaxSize);
		lua_setfield(pLuaState, -2, "max_lenght");
	}
	else if(pConfigVariable->m_Type == SConfigVariable::EVariableType::VAR_COLOR)
	{
		const auto *pColorConfigVariable = (const SColorConfigVariable *)pConfigVariable;

		auto Color = ColorHSLA(pColorConfigVariable->m_Default, true);

		PushColor(pLuaState, Color, pColorConfigVariable->m_Alpha);
		lua_setfield(pLuaState, -2, "default");

		lua_pushboolean(pLuaState, pColorConfigVariable->m_Light);
		lua_setfield(pLuaState, -2, "light");

		lua_pushboolean(pLuaState, pColorConfigVariable->m_Alpha);
		lua_setfield(pLuaState, -2, "alpha");
	}
	else
	{
		lua_pushnil(pLuaState);
	}

	return 1;
}

int CLua::LuaGetGameTick(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 0)
		return luaL_error(pLuaState, "expected 0 arguments");

	CLua *pLua = GetSelf(pLuaState);

	lua_pushinteger(pLuaState, pLua->Client()->GameTick(g_Config.m_ClDummy));

	return 1;
}

int CLua::LuaGetGameTickSpeed(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 0)
		return luaL_error(pLuaState, "expected 0 arguments");

	CLua *pLua = GetSelf(pLuaState);

	lua_pushinteger(pLuaState, pLua->Client()->GameTickSpeed());

	return 1;
}

int CLua::LuaGetClientData(lua_State *pLuaState)
{
	if(lua_gettop(pLuaState) != 1)
		return luaL_error(pLuaState, "expected 1 arguments");

	CLua *pLua = GetSelf(pLuaState);

	int ClientId = luaL_checkinteger(pLuaState, 1);

	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return luaL_error(pLuaState, "invalid client id");

	CGameClient::CClientData *pClient = &pLua->m_pClient->m_aClients[ClientId];

	lua_newtable(pLuaState);

	lua_pushinteger(pLuaState, pClient->m_UseCustomColor);
	lua_setfield(pLuaState, -2, "use_custom_color");

	PushColor(pLuaState, ColorHSLA(pClient->m_ColorBody), true);
	lua_setfield(pLuaState, -2, "color_body");

	PushColor(pLuaState, ColorHSLA(pClient->m_ColorFeet), true);
	lua_setfield(pLuaState, -2, "color_feet");


	lua_pushstring(pLuaState, pClient->m_aName);
	lua_setfield(pLuaState, -2, "name");

	lua_pushstring(pLuaState, pClient->m_aClan);
	lua_setfield(pLuaState, -2, "clan");

	lua_pushinteger(pLuaState, pClient->m_Country);
	lua_setfield(pLuaState, -2, "country");

	lua_pushstring(pLuaState, pClient->m_aSkinName);
	lua_setfield(pLuaState, -2, "skin_name");

	PushColor(pLuaState, pClient->m_SkinColor, true);
	lua_setfield(pLuaState, -2, "skin_color");

	lua_pushinteger(pLuaState, pClient->m_Team);
	lua_setfield(pLuaState, -2, "team");

	lua_pushinteger(pLuaState, pClient->m_Emoticon);
	lua_setfield(pLuaState, -2, "emoticon");

	lua_pushinteger(pLuaState, pClient->m_EmoticonStartTick);
	lua_setfield(pLuaState, -2, "emoticon_start_tick");
	

	lua_pushboolean(pLuaState, pClient->m_Solo);
	lua_setfield(pLuaState, -2, "solo");

	lua_pushboolean(pLuaState, pClient->m_Jetpack);
	lua_setfield(pLuaState, -2, "jetpack");

	lua_pushboolean(pLuaState, pClient->m_CollisionDisabled);
	lua_setfield(pLuaState, -2, "collision_disabled");

	lua_pushboolean(pLuaState, pClient->m_EndlessHook);
	lua_setfield(pLuaState, -2, "endless_hook");

	lua_pushboolean(pLuaState, pClient->m_EndlessJump);
	lua_setfield(pLuaState, -2, "endless_jump");

	lua_pushboolean(pLuaState, pClient->m_HammerHitDisabled);
	lua_setfield(pLuaState, -2, "hammer_hit_disabled");

	lua_pushboolean(pLuaState, pClient->m_GrenadeHitDisabled);
	lua_setfield(pLuaState, -2, "grenade_hit_disabled");

	lua_pushboolean(pLuaState, pClient->m_LaserHitDisabled);
	lua_setfield(pLuaState, -2, "laser_hit_disabled");

	lua_pushboolean(pLuaState, pClient->m_ShotgunHitDisabled);
	lua_setfield(pLuaState, -2, "shotgun_hit_disabled");

	lua_pushboolean(pLuaState, pClient->m_HookHitDisabled);
	lua_setfield(pLuaState, -2, "hook_hit_disabled");

	lua_pushboolean(pLuaState, pClient->m_Super);
	lua_setfield(pLuaState, -2, "super");

	lua_pushboolean(pLuaState, pClient->m_HasTelegunGun);
	lua_setfield(pLuaState, -2, "has_telegun_gun");

	lua_pushboolean(pLuaState, pClient->m_HasTelegunGrenade);
	lua_setfield(pLuaState, -2, "has_telegun_grenade");

	lua_pushboolean(pLuaState, pClient->m_HasTelegunLaser);
	lua_setfield(pLuaState, -2, "has_telegun_laser");

	lua_pushinteger(pLuaState, pClient->m_FreezeEnd);
	lua_setfield(pLuaState, -2, "freeze_end");

	lua_pushboolean(pLuaState, pClient->m_DeepFrozen);
	lua_setfield(pLuaState, -2, "deep_frozen");

	lua_pushboolean(pLuaState, pClient->m_LiveFrozen);
	lua_setfield(pLuaState, -2, "live_frozen");

	
	lua_pushnumber(pLuaState, pClient->m_Angle);
	lua_setfield(pLuaState, -2, "angle");

	lua_pushboolean(pLuaState, pClient->m_Active);
	lua_setfield(pLuaState, -2, "active");

	lua_pushboolean(pLuaState, pClient->m_ChatIgnore);
	lua_setfield(pLuaState, -2, "chat_ignore");

	lua_pushboolean(pLuaState, pClient->m_EmoticonIgnore);
	lua_setfield(pLuaState, -2, "emoticon_ignore");

	lua_pushboolean(pLuaState, pClient->m_Friend);
	lua_setfield(pLuaState, -2, "friend");

	lua_pushboolean(pLuaState, pClient->m_Foe);
	lua_setfield(pLuaState, -2, "foe");


	lua_pushinteger(pLuaState, pClient->m_AuthLevel);
	lua_setfield(pLuaState, -2, "auth_level");

	lua_pushboolean(pLuaState, pClient->m_Afk);
	lua_setfield(pLuaState, -2, "afk");

	lua_pushboolean(pLuaState, pClient->m_Paused);
	lua_setfield(pLuaState, -2, "paused");

	lua_pushboolean(pLuaState, pClient->m_Spec);
	lua_setfield(pLuaState, -2, "spec");


	lua_newtable(pLuaState);
	for(size_t i = 0; i < sizeof(pClient->m_aSwitchStates) / sizeof(pClient->m_aSwitchStates[0]); i++)
	{
		lua_pushboolean(pLuaState, pClient->m_aSwitchStates[i]);
		lua_rawseti(pLuaState, -2, i + 1);
	}
	lua_setfield(pLuaState, -2, "switch_states");

	return 1;
}

int CLua::LuaGetClientStats(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 1)
		return luaL_error(pLuaState, "expected 1 arguments");

	CLua *pLua = GetSelf(pLuaState);

	int ClientId = luaL_checkinteger(pLuaState, 1);

	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return luaL_error(pLuaState, "invalid client id");

	CGameClient::CClientStats *pStats = &pLua->m_pClient->m_aStats[ClientId];

	lua_newtable(pLuaState);

	lua_newtable(pLuaState);
	for(int i = 0; i < NUM_WEAPONS; i++)
	{
		lua_pushinteger(pLuaState, pStats->m_aFragsWith[i]);
		lua_rawseti(pLuaState, -2, i + 1);
	}
	lua_setfield(pLuaState, -2, "frags_with");

	lua_newtable(pLuaState);
	for(int i = 0; i < NUM_WEAPONS; i++)
	{
		lua_pushinteger(pLuaState, pStats->m_aDeathsFrom[i]);
		lua_rawseti(pLuaState, -2, i + 1);
	}
	lua_setfield(pLuaState, -2, "deaths_from");

	lua_pushinteger(pLuaState, pStats->m_Frags);
	lua_setfield(pLuaState, -2, "frags");

	lua_pushinteger(pLuaState, pStats->m_Deaths);
	lua_setfield(pLuaState, -2, "deaths");

	lua_pushinteger(pLuaState, pStats->m_Suicides);
	lua_setfield(pLuaState, -2, "suicides");

	lua_pushinteger(pLuaState, pStats->m_BestSpree);
	lua_setfield(pLuaState, -2, "best_spree");

	lua_pushinteger(pLuaState, pStats->m_CurrentSpree);
	lua_setfield(pLuaState, -2, "current_spree");


	lua_pushinteger(pLuaState, pStats->m_FlagGrabs);
	lua_setfield(pLuaState, -2, "flag_grabs");

	lua_pushinteger(pLuaState, pStats->m_FlagCaptures);
	lua_setfield(pLuaState, -2, "flag_captures");

	return 1;
}

int CLua::LuaGetClientTeam(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 1)
		return luaL_error(pLuaState, "expected 1 arguments");

	CLua *pLua = GetSelf(pLuaState);

	int ClientId = luaL_checkinteger(pLuaState, 1);

	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return luaL_error(pLuaState, "invalid client id");

	lua_pushinteger(pLuaState, pLua->GameClient()->m_Teams.Team(ClientId));

	return 1;
}

int CLua::LuaGetLocalId(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 0)
		return luaL_error(pLuaState, "expected 0 arguments");

	CLua *pLua = GetSelf(pLuaState);

	lua_pushinteger(pLuaState, pLua->m_pClient->m_aLocalIds[0]);

	return 1;
}

int CLua::LuaGetLocalDummyId(lua_State *pLuaState)
{
	if(lua_gettop(pLuaState) != 0)
		return luaL_error(pLuaState, "expected 0 arguments");

	CLua *pLua = GetSelf(pLuaState);

	if(pLua->Client()->DummyConnected())
		lua_pushinteger(pLuaState, pLua->m_pClient->m_aLocalIds[1]);
	else
		lua_pushnil(pLuaState);

	return 1;
}

int CLua::LuaGetLocalName(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 0)
		return luaL_error(pLuaState, "expected 0 arguments");

	CLua *pLua = GetSelf(pLuaState);

	lua_pushstring(pLuaState, pLua->Client()->PlayerName());

	return 1;
}

int CLua::LuaGetLocalDummyName(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 0)
		return luaL_error(pLuaState, "expected 0 arguments");

	CLua *pLua = GetSelf(pLuaState);

	lua_pushstring(pLuaState, pLua->Client()->DummyName());

	return 1;
}

int CLua::LuaIsDummyControlled(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 0)
		return luaL_error(pLuaState, "expected 0 arguments");

	CLua *pLua = GetSelf(pLuaState);

	lua_pushboolean(pLuaState, pLua->Config()->m_ClDummy > 0 && pLua->Client()->DummyConnected());

	return 1;
}

int CLua::LuaToggleDummyControl(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 0)
		return luaL_error(pLuaState, "expected 0 arguments");

	CLua *pLua = GetSelf(pLuaState);

	pLua->Config()->m_ClDummy = pLua->Config()->m_ClDummy > 0 ? 0 : 1;

	return 0;
}

int CLua::LuaSendInfo(lua_State *pLuaState)
{
	if(lua_gettop(pLuaState) != 0)
		return luaL_error(pLuaState, "expected 0 arguments");

	CLua *pLua = GetSelf(pLuaState);

	pLua->m_pClient->SendInfo(false);

	return 0;
}

int CLua::LuaSendDummyInfo(lua_State *pLuaState)
{
	if(lua_gettop(pLuaState) != 0)
		return luaL_error(pLuaState, "expected 0 arguments");

	CLua *pLua = GetSelf(pLuaState);

	pLua->m_pClient->SendDummyInfo(false);

	return 0;
}

int CLua::LuaGetState(lua_State *pLuaState)
{
	if(lua_gettop(pLuaState) != 0)
		return luaL_error(pLuaState, "expected 0 arguments");

	CLua *pLua = GetSelf(pLuaState);

	lua_pushinteger(pLuaState, pLua->Client()->State());

	return 1;
}

int CLua::LuaConnect(lua_State *pLuaState)
{
	if(lua_gettop(pLuaState) < 1 || lua_gettop(pLuaState) > 2)
		return luaL_error(pLuaState, "expected 1 or 2 arguments");

	CLua *pLua = GetSelf(pLuaState);

	const char *pAddress = luaL_checkstring(pLuaState, 1);

	const char *pPassword = nullptr;

	if(lua_gettop(pLuaState) == 2)
		pPassword = luaL_checkstring(pLuaState, 2);

	pLua->Client()->Connect(pAddress, pPassword);

	return 0;
}

int CLua::LuaConnectDummy(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 0)
		return luaL_error(pLuaState, "expected 0 arguments");

	CLua *pLua = GetSelf(pLuaState);

	pLua->Client()->DummyConnect();

	return 0;
}

int CLua::LuaDisconnect(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 0)
		return luaL_error(pLuaState, "expected 0 arguments");

	CLua *pLua = GetSelf(pLuaState);

	DISABLE_IN_EVENT(LUA_EVENT_TYPE_ON_NEW_SNAPSHOT); // TODO: remove this (when we call this function on new snapshot app crashes)

	pLua->Client()->Disconnect();

	return 0;
}

int CLua::LuaDisconnectDummy(lua_State *pLuaState)
{
	if(lua_gettop(pLuaState) < 0 || lua_gettop(pLuaState) > 1)
		return luaL_error(pLuaState, "expected 0 or 1 arguments");

	CLua *pLua = GetSelf(pLuaState);

	const char *pReason = nullptr;

	if (lua_gettop(pLuaState) == 1 && !lua_isnil(pLuaState, 1))
	{
		pReason = luaL_checkstring(pLuaState, 1);
	}

	pLua->Client()->DummyDisconnect(pReason);

	return 0;
}

int CLua::LuaIsDummyAllowed(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 0)
		return luaL_error(pLuaState, "expected 0 arguments");

	CLua *pLua = GetSelf(pLuaState);

	lua_pushboolean(pLuaState, pLua->Client()->DummyAllowed());

	return 1;
}

int CLua::LuaIsDummyConnected(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 0)
		return luaL_error(pLuaState, "expected 0 arguments");

	CLua *pLua = GetSelf(pLuaState);

	lua_pushboolean(pLuaState, pLua->Client()->DummyConnected());

	return 1;
}

int CLua::LuaIsDummyConnecting(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 0)
		return luaL_error(pLuaState, "expected 0 arguments");

	CLua *pLua = GetSelf(pLuaState);

	lua_pushboolean(pLuaState, pLua->Client()->DummyConnecting());

	return 1;
}

int CLua::LuaQuit(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 0)
		return luaL_error(pLuaState, "expected 0 arguments");

	CLua *pLua = GetSelf(pLuaState);

	pLua->Client()->Quit();

	return 0;
}

int CLua::LuaRestart(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 0)
		return luaL_error(pLuaState, "expected 0 arguments");

	CLua *pLua = GetSelf(pLuaState);

	pLua->Client()->Restart();

	return 0;
}

int CLua::LuaKill(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 0)
		return luaL_error(pLuaState, "expected 0 arguments");

	CLua *pLua = GetSelf(pLuaState);

	pLua->m_pClient->SendKill(-1);

	return 0;
}

int CLua::LuaNewUIRect(lua_State *pLuaState)
{
	if(lua_gettop(pLuaState) != 0)
		return luaL_error(pLuaState, "expected 0 arguments");

	PushUIRect(pLuaState, CUIRect());

	return 1;
}

int CLua::LuaGetScreenUIRect(lua_State *pLuaState)
{
	if(lua_gettop(pLuaState) != 0)
		return luaL_error(pLuaState, "expected 0 arguments");

	CLua *pLua = GetSelf(pLuaState);

	lua_newtable(pLuaState);

	SetUIRect(pLuaState, *pLua->Ui()->Screen(), -1);

	luaL_getmetatable(pLuaState, "UIRectMetatable");
	lua_setmetatable(pLuaState, -2);

	return 1;
}

int CLua::LuaMapScreen(lua_State *pLuaState)
{
	if(lua_gettop(pLuaState) != 0)
		return luaL_error(pLuaState, "expected 0 arguments");

	CLua *pLua = GetSelf(pLuaState);

	pLua->Ui()->MapScreen();

	return 0;
}

int CLua::LuaPixelSize(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 0)
		return luaL_error(pLuaState, "expected 0 arguments");

	CLua *pLua = GetSelf(pLuaState);

	lua_pushnumber(pLuaState, pLua->Ui()->PixelSize());

	return 1;
}

int CLua::BasicLuaUIRectSplitMid(lua_State *pLuaState, UIRECTSPLITMID pfnCallback)
{
	if(lua_gettop(pLuaState) < 3 || lua_gettop(pLuaState) > 4)
		return luaL_error(pLuaState, "expected 2 or 3 arguments");

	CUIRect ThisRect = CheckUIRect(pLuaState, 1);

	CUIRect Rect1;
	CUIRect Rect2;

	CUIRect *pRect1 = &Rect1;
	CUIRect *pRect2 = &Rect2;

	if(lua_isnil(pLuaState, 2))
		pRect1 = nullptr;
	else
	{
		auto Rect = GetUIRect(pLuaState, 2);

		if(!Rect)
			return luaL_error(pLuaState, "expected rect or nil on 1nd argument");

		Rect1 = *Rect;
	}

	if(lua_isnil(pLuaState, 3))
		pRect2 = nullptr;
	else
	{
		auto Rect = GetUIRect(pLuaState, 3);

		if(!Rect)
			return luaL_error(pLuaState, "expected rect or nil on 2rd argument");

		Rect2 = *Rect;
	}
	

	if(lua_gettop(pLuaState) == 3)
		(ThisRect.*pfnCallback)(pRect1, pRect2, 0);
	else
	{
		float SplitValue = luaL_checknumber(pLuaState, 4);

		(ThisRect.*pfnCallback)(pRect1, pRect2, SplitValue);
	}


	if(pRect1)
		SetUIRect(pLuaState, *pRect1, 2);

	if(pRect2)
		SetUIRect(pLuaState, *pRect2, 3);

	return 0;
}

int CLua::BasicLuaUIRectSplitSide(lua_State* pLuaState, UIREACTSPLITSIDE pfnCallback)
{
	if(lua_gettop(pLuaState) != 4)
		return luaL_error(pLuaState, "expected 3 arguments");

	float Cut = luaL_checknumber(pLuaState, 2);

	CUIRect ThisRect = CheckUIRect(pLuaState, 1);

	CUIRect Rect1;
	CUIRect Rect2;

	CUIRect *pRect1 = &Rect1;
	CUIRect *pRect2 = &Rect2;
	
	if(lua_isnil(pLuaState, 3))
		pRect1 = nullptr;
	else
	{
		auto Rect = GetUIRect(pLuaState, 3);
		if(!Rect)
			return luaL_error(pLuaState, "expected rect or nil on 2nd argument");
		Rect1 = *Rect;
	}
	
	if(lua_isnil(pLuaState, 4))
		pRect2 = nullptr;
	else
	{
		auto Rect = GetUIRect(pLuaState, 4);
		if(!Rect)
			return luaL_error(pLuaState, "expected rect or nil on 3rd argument");
		Rect2 = *Rect;
	}
	

	(ThisRect.*pfnCallback)(Cut, pRect1, pRect2);


	if(pRect1)
		SetUIRect(pLuaState, *pRect1, 3);

	if(pRect2)
		SetUIRect(pLuaState, *pRect2, 4);

	return 0;
}

int CLua::LuaUIRectMargin(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 3)
		return luaL_error(pLuaState, "expected 2 arguments");

	CUIRect ThisRect = CheckUIRect(pLuaState, 1);

	auto OptVec = GetVec2(pLuaState, 2);

	if(!(OptVec || lua_isnumber(pLuaState, 2)))
		return luaL_error(pLuaState, "expected vec2 or number on 1st argument");

	CUIRect Rect = CheckUIRect(pLuaState, 3);

	if(OptVec)
	{
		ThisRect.Margin(*OptVec, &Rect);
	}
	else
	{
		float Margin = luaL_checknumber(pLuaState, 2);

		ThisRect.Margin(Margin, &Rect);
	}

	SetUIRect(pLuaState, Rect, 3);

	return 0;
}

int CLua::BasicLuaUIRectMargin(lua_State *pLuaState, UIRECTMARGIN pfnCallback)
{
	if(lua_gettop(pLuaState) != 3)
		return luaL_error(pLuaState, "expected 2 arguments");

	CUIRect ThisRect = CheckUIRect(pLuaState, 1);

	float Margin = luaL_checknumber(pLuaState, 2);

	CUIRect Rect = CheckUIRect(pLuaState, 3);

	(ThisRect.*pfnCallback)(Margin, &Rect);

	SetUIRect(pLuaState, Rect, 3);

	return 0;
}

int CLua::LuaUIRectDraw(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 4)
		return luaL_error(pLuaState, "expected 3 arguments");

	CLua *pLua = GetSelf(pLuaState);
	ENABLE_IN_EVENT(LUA_EVENT_TYPE_ON_RENDER);

	CUIRect ThisRect = CheckUIRect(pLuaState, 1);

	ColorHSLA Color = CheckColor(pLuaState, 2);

	int Corners = luaL_checkinteger(pLuaState, 3);

	float Rounding = luaL_checknumber(pLuaState, 4);

	ThisRect.Draw(color_cast<ColorRGBA>(Color), Corners, Rounding);

	return 0;
}

int CLua::LuaUIRectDraw4(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 7)
		return luaL_error(pLuaState, "expected 6 arguments");

	CLua *pLua = GetSelf(pLuaState);
	ENABLE_IN_EVENT(LUA_EVENT_TYPE_ON_RENDER);

	CUIRect ThisRect = CheckUIRect(pLuaState, 1);

	ColorHSLA Color1 = CheckColor(pLuaState, 2);
	ColorHSLA Color2 = CheckColor(pLuaState, 3);
	ColorHSLA Color3 = CheckColor(pLuaState, 4);
	ColorHSLA Color4 = CheckColor(pLuaState, 5);

	int Corners = luaL_checkinteger(pLuaState, 6);

	float Rounding = luaL_checknumber(pLuaState, 7);

	ThisRect.Draw4(color_cast<ColorRGBA>(Color1), color_cast<ColorRGBA>(Color2), color_cast<ColorRGBA>(Color3), color_cast<ColorRGBA>(Color4), Corners, Rounding);

	return 0;
}

int CLua::LuaUIDoLabel(lua_State *pLuaState)
{
	if(lua_gettop(pLuaState) < 4 || lua_gettop(pLuaState) > 5)
		return luaL_error(pLuaState, "expected 4 or 5 arguments");

	CLua *pLua = GetSelf(pLuaState);
	ENABLE_IN_EVENT(LUA_EVENT_TYPE_ON_RENDER);

	CUIRect Rect = CheckUIRect(pLuaState, 1);

	const char *pText = luaL_checkstring(pLuaState, 2);

	float Size = luaL_checknumber(pLuaState, 3);

	int Align = luaL_checkinteger(pLuaState, 4);

	SLabelProperties Properties;

	if(auto OptProperties = GetLabelProperties(pLuaState, 5))
		Properties = *OptProperties;

	pLua->Ui()->DoLabel(&Rect, pText, Size, Align, Properties);

	return 0;
}

int CLua::LuaGetSnapshotNumberItems(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 1)
		return luaL_error(pLuaState, "expected 1 arguments");

	CLua *pLua = GetSelf(pLuaState);

	int Type = luaL_checkinteger(pLuaState, 1);

	if(Type < 0 || Type >= IClient::NUM_SNAPSHOT_TYPES)
		return luaL_error(pLuaState, "invalid snapshot type");

	lua_pushinteger(pLuaState, pLua->Client()->SnapNumItems(Type));

	return 1;
}

int CLua::LuaGetSnapshotItem(lua_State* pLuaState)
{
	if(lua_gettop(pLuaState) != 1)
		return luaL_error(pLuaState, "expected 1 arguments");

	CLua *pLua = GetSelf(pLuaState);

	int Type = luaL_checkinteger(pLuaState, 1);

	if(Type < 0 || Type >= IClient::NUM_SNAPSHOT_TYPES)
		return luaL_error(pLuaState, "invalid snapshot type");

	int Index = luaL_checkinteger(pLuaState, 2);

	if(Index < 0 || Index >= pLua->Client()->SnapNumItems(Type))
		return luaL_error(pLuaState, "invalid snapshot index");

	IClient::CSnapItem Item = pLua->Client()->SnapGetItem(Type, Index);

	lua_newtable(pLuaState);

	lua_pushinteger(pLuaState, Item.m_Type);
	lua_setfield(pLuaState, -2, "type");

	lua_pushinteger(pLuaState, Item.m_Id);
	lua_setfield(pLuaState, -2, "id");

	switch (Item.m_Type)
	{
	case NETOBJTYPE_EX:
	{
		break;
	}
	case NETOBJTYPE_PLAYERINPUT:
	{
		CNetObj_PlayerInput *pPlayerInput = (CNetObj_PlayerInput *)Item.m_pData;

		lua_pushinteger(pLuaState, pPlayerInput->m_Direction);
		lua_setfield(pLuaState, -2, "direction");

		lua_pushinteger(pLuaState, pPlayerInput->m_TargetX);
		lua_setfield(pLuaState, -2, "target_x");

		lua_pushinteger(pLuaState, pPlayerInput->m_TargetY);
		lua_setfield(pLuaState, -2, "target_y");

		lua_pushinteger(pLuaState, pPlayerInput->m_Jump);
		lua_setfield(pLuaState, -2, "jump");

		lua_pushinteger(pLuaState, pPlayerInput->m_Fire);
		lua_setfield(pLuaState, -2, "fire");

		lua_pushinteger(pLuaState, pPlayerInput->m_Hook);
		lua_setfield(pLuaState, -2, "hook");

		lua_pushinteger(pLuaState, pPlayerInput->m_PlayerFlags);
		lua_setfield(pLuaState, -2, "player_flags");

		lua_pushinteger(pLuaState, pPlayerInput->m_WantedWeapon);
		lua_setfield(pLuaState, -2, "wanted_weapon");

		lua_pushinteger(pLuaState, pPlayerInput->m_NextWeapon);
		lua_setfield(pLuaState, -2, "next_weapon");

		lua_pushinteger(pLuaState, pPlayerInput->m_PrevWeapon);
		lua_setfield(pLuaState, -2, "prev_weapon");

		break;
	}
	case NETOBJTYPE_PROJECTILE:
	{
		CNetObj_Projectile *pProjectile = (CNetObj_Projectile *)Item.m_pData;

		lua_pushinteger(pLuaState, pProjectile->m_X);
		lua_setfield(pLuaState, -2, "x");

		lua_pushinteger(pLuaState, pProjectile->m_Y);
		lua_setfield(pLuaState, -2, "y");

		lua_pushinteger(pLuaState, pProjectile->m_VelX);
		lua_setfield(pLuaState, -2, "vel_x");

		lua_pushinteger(pLuaState, pProjectile->m_VelY);
		lua_setfield(pLuaState, -2, "vel_y");

		lua_pushinteger(pLuaState, pProjectile->m_Type);
		lua_setfield(pLuaState, -2, "type");

		lua_pushinteger(pLuaState, pProjectile->m_StartTick);
		lua_setfield(pLuaState, -2, "start_tick");

		break;
	}
	case NETOBJTYPE_LASER:
	{
		CNetObj_Laser *pLaser = (CNetObj_Laser *)Item.m_pData;

		lua_pushinteger(pLuaState, pLaser->m_X);
		lua_setfield(pLuaState, -2, "x");

		lua_pushinteger(pLuaState, pLaser->m_Y);
		lua_setfield(pLuaState, -2, "y");

		lua_pushinteger(pLuaState, pLaser->m_FromX);
		lua_setfield(pLuaState, -2, "from_x");

		lua_pushinteger(pLuaState, pLaser->m_FromY);
		lua_setfield(pLuaState, -2, "from_y");

		lua_pushinteger(pLuaState, pLaser->m_StartTick);
		lua_setfield(pLuaState, -2, "start_tick");

		break;
	}
	case NETOBJTYPE_PICKUP:
	{
		CNetObj_Pickup *pPickup = (CNetObj_Pickup *)Item.m_pData;

		lua_pushinteger(pLuaState, pPickup->m_X);
		lua_setfield(pLuaState, -2, "x");

		lua_pushinteger(pLuaState, pPickup->m_Y);
		lua_setfield(pLuaState, -2, "y");

		lua_pushinteger(pLuaState, pPickup->m_Type);
		lua_setfield(pLuaState, -2, "type");

		lua_pushinteger(pLuaState, pPickup->m_Subtype);
		lua_setfield(pLuaState, -2, "subtype");

		break;
	}
	case NETOBJTYPE_FLAG:
	{
		CNetObj_Flag *pFlag = (CNetObj_Flag *)Item.m_pData;

		lua_pushinteger(pLuaState, pFlag->m_X);
		lua_setfield(pLuaState, -2, "x");

		lua_pushinteger(pLuaState, pFlag->m_Y);
		lua_setfield(pLuaState, -2, "y");

		lua_pushinteger(pLuaState, pFlag->m_Team);
		lua_setfield(pLuaState, -2, "team");

		break;
	}
	case NETOBJTYPE_GAMEINFO:
	{
		CNetObj_GameInfo *pGameInfo = (CNetObj_GameInfo *)Item.m_pData;

		lua_pushinteger(pLuaState, pGameInfo->m_GameFlags);
		lua_setfield(pLuaState, -2, "game_flags");

		lua_pushinteger(pLuaState, pGameInfo->m_GameStateFlags);
		lua_setfield(pLuaState, -2, "game_state_flags");

		lua_pushinteger(pLuaState, pGameInfo->m_RoundStartTick);
		lua_setfield(pLuaState, -2, "round_start_tick");

		lua_pushinteger(pLuaState, pGameInfo->m_WarmupTimer);
		lua_setfield(pLuaState, -2, "warmup_timer");

		lua_pushinteger(pLuaState, pGameInfo->m_ScoreLimit);
		lua_setfield(pLuaState, -2, "score_limit");

		lua_pushinteger(pLuaState, pGameInfo->m_TimeLimit);
		lua_setfield(pLuaState, -2, "time_limit");

		lua_pushinteger(pLuaState, pGameInfo->m_RoundNum);
		lua_setfield(pLuaState, -2, "round_num");

		lua_pushinteger(pLuaState, pGameInfo->m_RoundCurrent);
		lua_setfield(pLuaState, -2, "round_current");

		break;
	}
	case NETOBJTYPE_GAMEDATA:
	{
		CNetObj_GameData *pGameData = (CNetObj_GameData *)Item.m_pData;

		lua_pushinteger(pLuaState, pGameData->m_TeamscoreRed);
		lua_setfield(pLuaState, -2, "teamscore_red");

		lua_pushinteger(pLuaState, pGameData->m_TeamscoreBlue);
		lua_setfield(pLuaState, -2, "teamscore_blue");

		lua_pushinteger(pLuaState, pGameData->m_FlagCarrierRed);
		lua_setfield(pLuaState, -2, "flag_carrier_red");

		lua_pushinteger(pLuaState, pGameData->m_FlagCarrierBlue);
		lua_setfield(pLuaState, -2, "flag_carrier_blue");

		break;
	}

	default: break;
	}




}

CUIRect CLua::CheckUIRect(lua_State* pLuaState, int Index)
{
	auto Rect = GetUIRect(pLuaState, Index);

	if(!Rect)
		luaL_error(pLuaState, "expected rect on %d argument", Index);

	return *Rect;
}

std::optional<CUIRect> CLua::GetUIRect(lua_State* pLuaState, int Index)
{
	if(!lua_istable(pLuaState, Index))
		return std::nullopt;

	lua_getfield(pLuaState, Index, "x");
	lua_getfield(pLuaState, Index, "y");
	lua_getfield(pLuaState, Index, "w");
	lua_getfield(pLuaState, Index, "h");

	if (!(lua_isnumber(pLuaState, -4) && lua_isnumber(pLuaState, -3) && lua_isnumber(pLuaState, -2) && lua_isnumber(pLuaState, -1)))
	{
		lua_pop(pLuaState, 4);
		return std::nullopt;
	}

	CUIRect Rect;
	Rect.x = lua_tonumber(pLuaState, -4);
	Rect.y = lua_tonumber(pLuaState, -3);
	Rect.w = lua_tonumber(pLuaState, -2);
	Rect.h = lua_tonumber(pLuaState, -1);

	lua_pop(pLuaState, 4);

	return Rect;
}

void CLua::SetUIRect(lua_State* pLuaState, const CUIRect& Rect, int Index)
{
	int NewIndex = Index > 0 ? Index : Index - 1;

	lua_pushnumber(pLuaState, Rect.x);
	lua_setfield(pLuaState, NewIndex, "x");

	lua_pushnumber(pLuaState, Rect.y);
	lua_setfield(pLuaState, NewIndex, "y");

	lua_pushnumber(pLuaState, Rect.w);
	lua_setfield(pLuaState, NewIndex, "w");

	lua_pushnumber(pLuaState, Rect.h);
	lua_setfield(pLuaState, NewIndex, "h");
}

void CLua::PushUIRect(lua_State* pLuaState, const CUIRect& Rect)
{
	lua_newtable(pLuaState);

	SetUIRect(pLuaState, Rect, -1);

	luaL_getmetatable(pLuaState, "UIRectMetatable");
	lua_setmetatable(pLuaState, -2);
}

ColorHSLA CLua::CheckColor(lua_State *pLuaState, int Index)
{
	auto Color = GetColor(pLuaState, Index);

	if(!Color)
		return luaL_error(pLuaState, "expected color on %d argument", Index);

	return *Color;
}

std::optional<ColorHSLA> CLua::GetColor(lua_State *pLuaState, int Index)
{
	if(lua_istable(pLuaState, Index))
	{
		lua_getfield(pLuaState, Index, "r");
		lua_getfield(pLuaState, Index, "g");
		lua_getfield(pLuaState, Index, "b");

		if(lua_isnumber(pLuaState, -3) && lua_isnumber(pLuaState, -2) && lua_isnumber(pLuaState, -1))
		{
			lua_getfield(pLuaState, Index, "a");
			if(lua_isnumber(pLuaState, -1))
			{
				auto Color = ColorRGBA(lua_tonumber(pLuaState, -4), lua_tonumber(pLuaState, -3), lua_tonumber(pLuaState, -2), lua_tonumber(pLuaState, -1));
				lua_pop(pLuaState, 4);
				return color_cast<ColorHSLA>(Color);
			}
			else
			{
				auto Color = ColorRGBA(lua_tonumber(pLuaState, -4), lua_tonumber(pLuaState, -3), lua_tonumber(pLuaState, -2));
				lua_pop(pLuaState, 4);
				return color_cast<ColorHSLA>(Color);
			}
		}

		lua_pop(pLuaState, 3);


		lua_getfield(pLuaState, Index, "h");
		lua_getfield(pLuaState, Index, "s");
		lua_getfield(pLuaState, Index, "l");

		if(lua_isnumber(pLuaState, -3) && lua_isnumber(pLuaState, -2) && lua_isnumber(pLuaState, -1))
		{
			lua_getfield(pLuaState, Index, "a");
			if (lua_isnumber(pLuaState, -1))
			{
				auto Color = ColorHSLA(lua_tonumber(pLuaState, -4), lua_tonumber(pLuaState, -3), lua_tonumber(pLuaState, -2), lua_tonumber(pLuaState, -1));
				lua_pop(pLuaState, 4);
				return Color;
			}
			else
			{
				auto Color = ColorHSLA(lua_tonumber(pLuaState, -4), lua_tonumber(pLuaState, -3), lua_tonumber(pLuaState, -2));
				lua_pop(pLuaState, 4);
				return Color;
			}
		}

		lua_pop(pLuaState, 3);
	}

	return std::nullopt;
}

void CLua::PushColor(lua_State *pLuaState, const ColorHSLA& Color, bool Alpha)
{
	lua_newtable(pLuaState);

	lua_pushnumber(pLuaState, Color.h);
	lua_setfield(pLuaState, -2, "h");

	lua_pushnumber(pLuaState, Color.s);
	lua_setfield(pLuaState, -2, "s");

	lua_pushnumber(pLuaState, Color.l);
	lua_setfield(pLuaState, -2, "l");

	if (Alpha)
	{
		lua_pushnumber(pLuaState, Color.a);
		lua_setfield(pLuaState, -2, "a");
	}
}

std::optional<vec2> CLua::GetVec2(lua_State *pLuaState, int Index)
{
	if (lua_istable(pLuaState, Index))
	{
		lua_getfield(pLuaState, Index, "x");
		lua_getfield(pLuaState, Index, "y");

		if (lua_isnumber(pLuaState, -2) && lua_isnumber(pLuaState, -1))
		{
			vec2 Vec;
			Vec.x = lua_tonumber(pLuaState, -2);
			Vec.y = lua_tonumber(pLuaState, -1);

			lua_pop(pLuaState, 2);

			return Vec;
		}

		lua_pop(pLuaState, 2);
	}

	return std::nullopt;
}

std::optional<SLabelProperties> CLua::GetLabelProperties(lua_State* pLuaState, int Index)
{
	if(lua_istable(pLuaState, Index))
	{
		lua_getfield(pLuaState, Index, "max_width");
		lua_getfield(pLuaState, Index, "stop_at_end");
		lua_getfield(pLuaState, Index, "ellipsis_at_end");
		lua_getfield(pLuaState, Index, "enable_width_check");
		lua_getfield(pLuaState, Index, "color_splits");

		if(lua_isnumber(pLuaState, -5) && lua_isboolean(pLuaState, -4) && lua_isboolean(pLuaState, -3) && lua_isboolean(pLuaState, -2) && lua_istable(pLuaState, -1))
		{
			SLabelProperties Properties;
			Properties.m_MaxWidth = lua_tonumber(pLuaState, -5);
			Properties.m_StopAtEnd = lua_toboolean(pLuaState, -4);
			Properties.m_EllipsisAtEnd = lua_toboolean(pLuaState, -3);
			Properties.m_EnableWidthCheck = lua_toboolean(pLuaState, -2);

			lua_pushnil(pLuaState);

			while(lua_next(pLuaState, -2) != 0)
			{
				if(std::optional<STextColorSplit> ColorSplit = GetTextColorSplit(pLuaState, -1))
				{
					Properties.m_vColorSplits.push_back(*ColorSplit);
				}
				else
				{
					lua_pop(pLuaState, 5 + 2);
					return std::nullopt;
				}

				lua_pop(pLuaState, 1);
			}

			lua_pop(pLuaState, 5);

			return Properties;
		}

		lua_pop(pLuaState, 5);
	}

	return std::nullopt;
}

std::optional<STextColorSplit> CLua::GetTextColorSplit(lua_State *pLuaState, int Index)
{
	if(lua_istable(pLuaState, Index))
	{
		lua_getfield(pLuaState, Index, "char_index");
		lua_getfield(pLuaState, Index, "length");
		lua_getfield(pLuaState, Index, "color");

		if(lua_isinteger(pLuaState, -3) && lua_isinteger(pLuaState, -2) && lua_istable(pLuaState, -1))
		{
			auto Color = GetColor(pLuaState, -1);
			if (!Color)
			{
				lua_pop(pLuaState, 3);
				return std::nullopt;
			}

			STextColorSplit ColorSplit(lua_tointeger(pLuaState, -3), lua_tointeger(pLuaState, -2), color_cast<ColorRGBA>(*Color));

			lua_pop(pLuaState, 3);

			return ColorSplit;
		}

		lua_pop(pLuaState, 3);
	}

	return std::nullopt;
}*/
