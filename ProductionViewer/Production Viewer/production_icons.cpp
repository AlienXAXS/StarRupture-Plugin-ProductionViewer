#include "production_icons.h"
#include "Plugin Core/Helpers/plugin_helpers.h"
#include "production_fields.h"

namespace ProductionIcons
{
	void Init(IPluginSelf* self)
	{
		IPluginHooks* hooks = self ? self->hooks : nullptr;
		if (hooks && hooks->Images)
			hooks->Images->UseSets(self, PLUGIN_IMAGE_SET_ITEMS);
	}

	PluginImageId IdOf(const std::string& uniqueItemName)
	{
		IPluginHooks* hooks = GetHooks();
		IPluginSelf*  self  = GetSelf();
		if (!hooks || !hooks->Images || !self || !ProductionViewerFields::Icons.On())
			return 0;

		if (uniqueItemName.empty() || uniqueItemName == ProductionViewerFields::kUnknownItem)
			return 0;

		return hooks->Images->Item(self, uniqueItemName.c_str());
	}
}
