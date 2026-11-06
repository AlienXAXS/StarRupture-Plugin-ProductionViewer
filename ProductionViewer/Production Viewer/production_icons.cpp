#include "production_icons.h"
#include "Plugin Core/Helpers/plugin_helpers.h"
#include "production_fields.h"

#include "Chimera_classes.hpp"
#include "AuItems_classes.hpp"
#include "Engine_classes.hpp"

#include "item_icons.h"

namespace ProductionIcons
{
	namespace
	{
		ItemIcons::Loader g_icons;

		std::string UniqueNameOf(SDK::UAuItemDataBase* item)
		{
			const SDK::FName* name = ProductionViewerFields::ItemUniqueName.In(item);
			return name ? name->ToString() : std::string();
		}

		const SDK::FSlateBrush* IconOf(SDK::UAuItemDataBase* item)
		{
			return ProductionViewerFields::ItemIcon.In(item);
		}
	}

	void Init(IPluginSelf* self)
	{
		if (!ProductionViewerFields::ItemIcons.On())
		{
			LOG_WARN("ProductionIcons: item icons are off - the game update moved something they need");
			return;
		}

		g_icons.Init(self, "ProductionIcons", { &UniqueNameOf, &IconOf });
	}

	void Shutdown()
	{
		g_icons.Shutdown();
	}

	void Tick()
	{
		g_icons.Tick();
	}

	void Draw(IModLoaderImGui* imgui, IPluginImGuiTextures* textures,
		const std::string& uniqueItemName, float size, float gap)
	{
		g_icons.Draw(imgui, textures, uniqueItemName, size, gap);
	}
}
