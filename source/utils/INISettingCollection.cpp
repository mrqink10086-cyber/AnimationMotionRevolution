#include "utils/INISettingCollection.h"

#include "utils/Logger.h"

namespace utils
{
	INISettingCollection::INISettingCollection() noexcept
	{
		REL::Relocation<std::uintptr_t> __vTable(*reinterpret_cast<std::uintptr_t*>(this));

		// Use the Skyrim's INISettingCollection virtual functions
		static std::uintptr_t* skyrimsVTable = REL::Relocation<std::uintptr_t*>{ vTableId }.get();

		// Replace all except the destructor (index 0)
		for (int i = 1; i < 10; i++) {
			__vTable.write_vfunc(i, skyrimsVTable[i]);
		}
	}

	bool INISettingCollection::ReadFromFile(std::string_view a_fileName)
	{
		const auto iniPath = (std::filesystem::current_path() / "Data/SKSE/Plugins" / a_fileName).string();
		if (iniPath.size() >= sizeof(subKey)) {
			return false;
		}
		std::memcpy(subKey, iniPath.c_str(), iniPath.size() + 1);

		if (_this()->OpenHandle(false)) {
			_this()->ReadAllSettings();
			_this()->CloseHandle();

			return true;
		}

		return false;
	}
}
