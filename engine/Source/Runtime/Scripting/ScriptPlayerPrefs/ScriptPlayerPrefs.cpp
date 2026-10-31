#include "Runtime/Scripting/ScriptPlayerPrefs/ScriptPlayerPrefs.h"

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Resource/UserPreferences/UserPreferences.h"

UserPreferences* PlayerPrefs::ResolvePreferences()
{
    return GET_SYSTEM(UserPreferences);
}

void PlayerPrefs::SetInt(const eastl::string& key, int value)
{
    if (UserPreferences* prefs = ResolvePreferences(); prefs != nullptr)
    {
        prefs->SetInt(key.c_str(), value);
    }
}

void PlayerPrefs::SetFloat(const eastl::string& key, float value)
{
    if (UserPreferences* prefs = ResolvePreferences(); prefs != nullptr)
    {
        prefs->SetFloat(key.c_str(), value);
    }
}

void PlayerPrefs::SetString(const eastl::string& key, const eastl::string& value)
{
    if (UserPreferences* prefs = ResolvePreferences(); prefs != nullptr)
    {
        prefs->SetString(key.c_str(), value.c_str());
    }
}

void PlayerPrefs::SetBool(const eastl::string& key, bool value)
{
    if (UserPreferences* prefs = ResolvePreferences(); prefs != nullptr)
    {
        prefs->SetBool(key.c_str(), value);
    }
}

int PlayerPrefs::GetInt(const eastl::string& key, int default_value)
{
    if (UserPreferences* prefs = ResolvePreferences(); prefs != nullptr)
    {
        return prefs->GetInt(key.c_str(), default_value);
    }
    return default_value;
}

float PlayerPrefs::GetFloat(const eastl::string& key, float default_value)
{
    if (UserPreferences* prefs = ResolvePreferences(); prefs != nullptr)
    {
        return prefs->GetFloat(key.c_str(), default_value);
    }
    return default_value;
}

eastl::string PlayerPrefs::GetString(const eastl::string& key, const eastl::string& default_value)
{
    if (UserPreferences* prefs = ResolvePreferences(); prefs != nullptr)
    {
        return eastl::string(prefs->GetString(key.c_str(), default_value.c_str()).c_str());
    }
    return default_value;
}

bool PlayerPrefs::GetBool(const eastl::string& key, bool default_value)
{
    if (UserPreferences* prefs = ResolvePreferences(); prefs != nullptr)
    {
        return prefs->GetBool(key.c_str(), default_value);
    }
    return default_value;
}

bool PlayerPrefs::HasKey(const eastl::string& key)
{
    if (UserPreferences* prefs = ResolvePreferences(); prefs != nullptr)
    {
        return prefs->HasKey(key.c_str());
    }
    // 没有偏好系统时"没有任何键"是唯一诚实的答案 —— 说 true 会让脚本以为读得到。
    return false;
}

void PlayerPrefs::DeleteKey(const eastl::string& key)
{
    if (UserPreferences* prefs = ResolvePreferences(); prefs != nullptr)
    {
        prefs->DeleteKey(key.c_str());
    }
}

void PlayerPrefs::DeleteAll()
{
    if (UserPreferences* prefs = ResolvePreferences(); prefs != nullptr)
    {
        prefs->DeleteAll();
    }
}

void PlayerPrefs::Save()
{
    if (UserPreferences* prefs = ResolvePreferences(); prefs != nullptr)
    {
        prefs->save();
    }
}
