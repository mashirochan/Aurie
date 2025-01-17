#include "module.hpp"
#include <Psapi.h>

namespace Aurie
{
	AurieStatus Internal::MdpCreateModule(
		IN const fs::path& ImagePath, 
		IN HMODULE ImageModule,
		IN AurieEntry ModuleInitialize, 
		IN AurieEntry ModulePreinitialize,
		IN AurieEntry ModuleUnload,
		IN AurieLoaderEntry FrameworkInitialize,
		IN AurieModuleCallback ModuleOperationCallback,
		IN uint8_t BitFlags,
		OUT AurieModule& Module
	)
	{
		AurieModule temp_module;
		AurieStatus last_status = AURIE_SUCCESS;

		// Populate known fields first
		temp_module.Flags.Bitfield = BitFlags;
		temp_module.ImagePath = ImagePath;
		temp_module.ModuleInitialize = ModuleInitialize;
		temp_module.ModulePreinitialize = ModulePreinitialize;
		temp_module.FrameworkInitialize = FrameworkInitialize;
		temp_module.ModuleOperationCallback = ModuleOperationCallback;
		temp_module.ModuleUnload = ModuleUnload;

		last_status = MdpQueryModuleInformation(
			ImageModule,
			&temp_module.ImageBase.Pointer,
			&temp_module.ImageSize,
			&temp_module.ImageEntrypoint.Pointer
		);

		if (!AurieSuccess(last_status))
		{
			return last_status;
		}

		Module = std::move(temp_module);

		return AURIE_SUCCESS;
	}

	bool Internal::MdpIsModuleMarkedForPurge(
		IN AurieModule* Module
	)
	{
		return Module->Flags.MarkedForPurge;
	}

	void Internal::MdpMarkModuleForPurge(
		IN AurieModule* Module
	)
	{
		Module->Flags.MarkedForPurge = true;
	}

	void Internal::MdpPurgeMarkedModules()
	{
		// Loop through all the modules marked for purge
		for (auto& module : g_LdrModuleList)
		{
			// Unmap the module, but don't call the unload routine, and don't remove it from the list
			if (MdpIsModuleMarkedForPurge(&module))
				MdpUnmapImage(&module, false, false);
		}

		// Remove the now unloaded modules from our list
		// Note we can't do this in the for loop, since that'd invalidate the iterators
		g_LdrModuleList.remove_if(
			[](AurieModule& Module) -> bool
			{
				return MdpIsModuleMarkedForPurge(&Module);
			}
		);
	}

	AurieStatus Internal::MdpMapImage(
		IN const fs::path& ImagePath, 
		OUT HMODULE& ImageBase
	)
	{
		// If the file doesn't exist, we have nothing to map
		std::error_code ec;
		if (!fs::exists(ImagePath, ec))
			return AURIE_FILE_NOT_FOUND;

		AurieStatus last_status = AURIE_SUCCESS;
		unsigned short target_arch = 0, self_arch = 0;
		
		// Query the target image architecture
		last_status = PpQueryImageArchitecture(
			ImagePath,
			target_arch
		);

		if (!AurieSuccess(last_status))
			return last_status;

		// Query the current architecture
		last_status = PpGetCurrentArchitecture(
			self_arch
		);

		// If we fail to query the current architecture, we bail.
		if (!AurieSuccess(last_status))
			return last_status;

		// Don't try to load modules which are the wrong architecture
		if (target_arch != self_arch)
			return AURIE_INVALID_ARCH;

		// Make sure the image has the required exports
		bool has_framework_init = PpFindFileExportByName(ImagePath, "__AurieFrameworkInit") != 0;
		bool has_module_entry = PpFindFileExportByName(ImagePath, "ModuleInitialize") != 0;

		if (!has_framework_init || !has_module_entry)
			return AURIE_INVALID_SIGNATURE;

		AurieModule* potential_loaded_copy = nullptr;
		last_status = MdpLookupModuleByPath(
			ImagePath,
			potential_loaded_copy
		);

		// If there's a module that's already loaded from the same path, deny loading it twice
		if (AurieSuccess(last_status))
			return AURIE_OBJECT_ALREADY_EXISTS;

		// Load the image into memory and make sure we loaded it
		HMODULE image_module = LoadLibraryW(ImagePath.wstring().c_str());

		if (!image_module)
			return AURIE_EXTERNAL_ERROR;

		ImageBase = image_module;
		return AURIE_SUCCESS;
	}

	void Internal::MdpBuildModuleList(
		IN const fs::path& BaseFolder, 
		IN bool Recursive, 
		IN std::function<bool(const fs::directory_entry& Entry)> Predicate,
		OUT std::vector<fs::path>& Files
	)
	{
		std::error_code ec;
		Files.clear();

		if (Recursive)
		{
			for (auto& entry : fs::recursive_directory_iterator(BaseFolder, ec))
				if (Predicate(entry))
					Files.push_back(entry.path());

			return;
		}

		for (auto& entry : fs::recursive_directory_iterator(BaseFolder, ec))
			if (Predicate(entry))
				Files.push_back(entry.path());	
	}

	AurieModule* Internal::MdpAddModuleToList(
		IN AurieModule&& Module
	)
	{
		return &g_LdrModuleList.emplace_back(std::move(Module));
	}

	AurieStatus Internal::MdpQueryModuleInformation(
		IN HMODULE Module, 
		OPTIONAL OUT PVOID* ModuleBase,
		OPTIONAL OUT uint32_t* SizeOfModule, 
		OPTIONAL OUT PVOID* EntryPoint
	)
	{
		// Query the information by asking Windows
		MODULEINFO module_info = {};
		if (!GetModuleInformation(
			GetCurrentProcess(),
			Module,
			&module_info,
			sizeof(module_info)
		))
		{
			return AURIE_EXTERNAL_ERROR;
		}

		// Fill in what the caller wants
		if (ModuleBase)
			*ModuleBase = module_info.lpBaseOfDll;

		if (SizeOfModule)
			*SizeOfModule = module_info.SizeOfImage;

		if (EntryPoint)
			*EntryPoint = module_info.EntryPoint;

		return AURIE_SUCCESS;
	}

	fs::path& Internal::MdpGetImagePath(
		IN AurieModule* Module
	)
	{
		return Module->ImagePath;
	}

	AurieStatus Internal::MdpGetImageFolder(
		IN AurieModule* Module, 
		OUT fs::path& Path
	)
	{
		fs::path& module_path = Internal::MdpGetImagePath(Module);

		if (!module_path.has_parent_path())
			return AURIE_INVALID_PARAMETER;

		Path = module_path.parent_path();
		return AURIE_SUCCESS;
	}

	AurieStatus Internal::MdpGetNextModule(
		IN AurieModule* Module, 
		OUT AurieModule*& NextModule
	)
	{
		// Find the module in our list (gets an iterator)
		auto list_iterator = std::find(
			g_LdrModuleList.begin(),
			g_LdrModuleList.end(),
			*Module
		);

		// Make sure that module is indeed in our list
		if (list_iterator == std::end(g_LdrModuleList))
			return AURIE_INVALID_PARAMETER;

		// Compute the distance from the beginning of the list to the module
		size_t distance = std::distance(g_LdrModuleList.begin(), list_iterator);

		// Advance to the next element
		distance = (distance + 1) % g_LdrModuleList.size();
		AurieModule& next_module = *std::next(g_LdrModuleList.begin(), distance);

		NextModule = &next_module;
		
		return AURIE_SUCCESS;
	}

	PVOID Internal::MdpGetModuleBaseAddress(
		IN AurieModule* Module
	)
	{
		return Module->ImageBase.Pointer;
	}

	EXPORTED AurieStatus Internal::MdpLookupModuleByPath(
		IN const fs::path& ModulePath,
		OUT AurieModule*& Module
	)
	{
		auto iterator = std::find_if(
			g_LdrModuleList.begin(),
			g_LdrModuleList.end(),
			[ModulePath](const AurieModule& Module) -> bool
			{
				return Module.ImagePath == ModulePath;
			}
		);

		if (iterator == g_LdrModuleList.end())
			return AURIE_OBJECT_NOT_FOUND;

		Module = &(*iterator);
		
		return AURIE_SUCCESS;
	}

	// The ignoring of return values here is on purpose, we just have to power through
	// and unload / free what we can.
	AurieStatus Internal::MdpUnmapImage(
		IN AurieModule* Module,
		IN bool RemoveFromList,
		IN bool CallUnloadRoutine
	)
	{
		AurieStatus last_status = AURIE_SUCCESS;

		// We don't have to do anything else, since SafetyHook will handle everything for us.
		// Truly a GOATed library, thank you @localcc for telling me about it love ya
		Module->Hooks.clear();

		// Call the unload entry if needed
		if (CallUnloadRoutine)
		{
			last_status = MdpDispatchEntry(
				Module,
				Module->ModuleUnload
			);
		}

		// Remove the module's operation callback
		Module->ModuleOperationCallback = nullptr;

		// Destory all interfaces created by the module
		for (auto& module_interface : Module->InterfaceTable)
		{
			if (module_interface.Interface)
				module_interface.Interface->Destroy();
		}

		// Wipe them off the interface table
		// Note these can't be freed, they're allocated by the owner module
		Module->InterfaceTable.clear();

		// Free all memory allocated by the module (except persistent memory)
		for (auto& memory_allocation : Module->MemoryAllocations)
		{
			MmpFreeMemory(
				memory_allocation.OwnerModule,
				memory_allocation.AllocationBase,
				false
			);
		}

		// Remove all the allocation entries, they're now invalid
		Module->MemoryAllocations.clear();

		// Free the module
		FreeLibrary(Module->ImageBase.Module);

		// Remove the module from our list if needed
		if (RemoveFromList)
			g_LdrModuleList.remove(*Module);

		return last_status;
	}

	AurieStatus Internal::MdpDispatchEntry(
		IN AurieModule* Module,
		IN AurieEntry Entry
	)
	{
		// Ignore dispatch attempts for the initial module
		if (Module == g_ArInitialImage)
			return AURIE_SUCCESS;

		ObpDispatchModuleOperationCallbacks(
			Module, 
			Entry, 
			true
		);

		AurieStatus module_status = Module->FrameworkInitialize(
			g_ArInitialImage,
			PpGetFrameworkRoutine,
			Entry,
			MdpGetImagePath(Module),
			Module
		);

		ObpDispatchModuleOperationCallbacks(
			Module, 
			Entry, 
			false
		);

		return module_status;
	}

	void Internal::MdpMapFolder(
		IN const fs::path& Folder, 
		IN bool Recursive,
		OPTIONAL OUT size_t* NumberOfMappedModules
	)
	{
		std::vector<fs::path> modules_to_map;

		MdpBuildModuleList(
			Folder,
			Recursive,
			[](const fs::directory_entry& entry) -> bool
			{
				if (!entry.is_regular_file())
					return false;

				if (!entry.path().has_filename())
					return false;

				if (!entry.path().filename().has_extension())
					return false;

				if (entry.path().filename().extension().compare(L".dll"))
					return false;

				return true;
			},
			modules_to_map
		);

		std::sort(
			modules_to_map.begin(),
			modules_to_map.end()
		);

		size_t loaded_count = 0;
		for (auto& module : modules_to_map)
		{
			AurieModule* loaded_module = nullptr;

			if (AurieSuccess(MdMapImage(module, loaded_module)))
				loaded_count++;
		}

		if (NumberOfMappedModules)
			*NumberOfMappedModules = loaded_count;
	}

	AurieStatus MdMapImage(
		IN const fs::path& ImagePath, 
		OUT AurieModule*& Module
	)
	{
		AurieStatus last_status = AURIE_SUCCESS;
		HMODULE image_base = nullptr;

		// Map the image
		last_status = Internal::MdpMapImage(ImagePath, image_base);

		if (!AurieSuccess(last_status))
			return last_status;

		// Find all the required functions
		uintptr_t framework_init_offset = PpFindFileExportByName(ImagePath, "__AurieFrameworkInit");
		uintptr_t module_init_offset = PpFindFileExportByName(ImagePath, "ModuleInitialize");

		uintptr_t module_callback_offset = PpFindFileExportByName(ImagePath, "ModuleOperationCallback");
		uintptr_t module_preload_offset = PpFindFileExportByName(ImagePath, "ModulePreinitialize");
		uintptr_t module_unload_offset = PpFindFileExportByName(ImagePath, "ModuleUnload");

		AurieEntry module_init = reinterpret_cast<AurieEntry>((char*)image_base + module_init_offset);
		AurieEntry module_preload = reinterpret_cast<AurieEntry>((char*)image_base + module_preload_offset);
		AurieEntry module_unload = reinterpret_cast<AurieEntry>((char*)image_base + module_unload_offset);
		AurieLoaderEntry fwk_init = reinterpret_cast<AurieLoaderEntry>((char*)image_base + framework_init_offset);
		AurieModuleCallback module_callback = reinterpret_cast<AurieModuleCallback>((char*)image_base + module_callback_offset);

		// Verify image integrity
		last_status = Internal::MmpVerifyCallback(image_base, module_init);
		if (!AurieSuccess(last_status))
			return last_status;

		last_status = Internal::MmpVerifyCallback(image_base, fwk_init);
		if (!AurieSuccess(last_status))
			return last_status;

		// MdiMapImage checks for __aurie_fwk_init and ModuleInitialize, but doesn't check ModulePreinitialize since it's optional
		// If the offsets are null, the thing wasn't found, and we shouldn't try to call it
		if (!module_preload_offset)
			module_preload = nullptr;

		if (!module_unload_offset)
			module_unload = nullptr;

		if (!module_callback_offset)
			module_callback = nullptr;

		// Create the module object
		AurieModule module_object = {};
		last_status = Internal::MdpCreateModule(
			ImagePath,
			image_base,
			module_init,
			module_preload,
			module_unload,
			fwk_init,
			module_callback,
			0,
			module_object
		);

		// TODO: Invoke module load callbacks

		if (!AurieSuccess(last_status))
			return last_status;

		// Add it to our list of modules
		Module = Internal::MdpAddModuleToList(std::move(module_object));
		return AURIE_SUCCESS;
	}

	bool MdIsImageInitialized(
		IN AurieModule* Module
	)
	{
		return Module->Flags.IsInitialized;
	}

	AurieStatus MdMapFolder(
		IN const fs::path& FolderPath, 
		IN bool Recursive
	)
	{
		if (!fs::exists(FolderPath))
			return AURIE_FILE_NOT_FOUND;

		Internal::MdpMapFolder(
			FolderPath,
			Recursive,
			nullptr
		);

		return AURIE_SUCCESS;
	}

	AurieStatus MdGetImageFilename(
		IN AurieModule* Module, 
		OUT std::wstring& Filename
	)
	{
		auto& image_path = Internal::MdpGetImagePath(Module);

		if (!image_path.has_filename())
			return AURIE_INVALID_PARAMETER;

		Filename = image_path.filename().wstring();

		return AURIE_SUCCESS;
	}

	bool MdIsImagePreinitialized(
		IN AurieModule* Module
	)
	{
		return Module->Flags.IsPreloaded;
	}

	AurieStatus MdUnmapImage(
		IN AurieModule* Module
	)
	{
		if (Module == g_ArInitialImage)
			return AURIE_ACCESS_DENIED;

		return Internal::MdpUnmapImage(Module, true, true);
	}
}
