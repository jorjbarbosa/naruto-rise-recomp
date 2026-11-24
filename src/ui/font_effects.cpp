// The outline and shadow font effects with dp lengths (font_effects.h).

#include "ui/font_effects.h"

#include <RmlUi/Core.h>

#include <cmath>
#include <memory>
#include <string_view>
#include <vector>

namespace ui::font_effects
{
    namespace
    {
        // A property of the effect as RmlUi's instancer declares it.
        struct Field
        {
            const char* name;
            const char* defaultValue;
            const char* parser; // "length" fields are scaled
        };

        float g_ratio = 1.0f;

        // RmlUi's instancer of the same name, given the declaration with its lengths in pixels.
        class Instancer final : public Rml::FontEffectInstancer
        {
        public:
            Instancer(Rml::FontEffectInstancer* builtIn, std::vector<Field> fields, const char* shorthand)
                : builtIn_(builtIn)
            {
                const Rml::PropertySpecification& specification = builtIn->GetPropertySpecification();
                for (const Field& field : fields)
                {
                    bool length = std::string_view(field.parser) == "length";
                    Mapped mapped;
                    mapped.own = RegisterProperty(field.name, field.defaultValue, length).AddParser(field.parser).GetId();
                    mapped.builtIn = specification.GetProperty(field.name)->GetId();
                    mapped.length = length;
                    fields_.push_back(mapped);
                }
                RegisterShorthand("font-effect", shorthand, Rml::ShorthandType::FallThrough);
            }

            Rml::SharedPtr<Rml::FontEffect> InstanceFontEffect(const Rml::String& name,
                                                               const Rml::PropertyDictionary& properties) override
            {
                Rml::PropertyDictionary pixels;
                for (const Mapped& field : fields_)
                {
                    const Rml::Property* property = properties.GetProperty(field.own);
                    if (property == nullptr)
                        continue;
                    if (!field.length)
                    {
                        pixels.SetProperty(field.builtIn, *property);
                        continue;
                    }
                    float value = property->Get<float>();
                    if (property->unit == Rml::Unit::DP)
                        value *= g_ratio;
                    pixels.SetProperty(field.builtIn, Rml::Property(std::round(value), Rml::Unit::PX));
                }
                return builtIn_->InstanceFontEffect(name, pixels);
            }

        private:
            struct Mapped
            {
                Rml::PropertyId own;
                Rml::PropertyId builtIn;
                bool length;
            };

            Rml::FontEffectInstancer* builtIn_;
            std::vector<Mapped> fields_;
        };

        std::unique_ptr<Instancer> g_outline;
        std::unique_ptr<Instancer> g_shadow;
    }

    void Register()
    {
        // The fields and shorthands are RmlUi's own (FontEffectOutline.cpp, FontEffectShadow.cpp).
        if (Rml::FontEffectInstancer* outline = Rml::Factory::GetFontEffectInstancer("outline"))
        {
            g_outline = std::make_unique<Instancer>(
                outline, std::vector<Field>{ { "width", "1px", "length" }, { "color", "white", "color" } }, "width, color");
            Rml::Factory::RegisterFontEffectInstancer("outline", g_outline.get());
        }
        if (Rml::FontEffectInstancer* shadow = Rml::Factory::GetFontEffectInstancer("shadow"))
        {
            g_shadow = std::make_unique<Instancer>(
                shadow,
                std::vector<Field>{ { "offset-x", "0px", "length" }, { "offset-y", "0px", "length" }, { "color", "white", "color" } },
                "offset-x, offset-y, color");
            Rml::Factory::RegisterFontEffectInstancer("shadow", g_shadow.get());
        }
    }

    bool SetDensityRatio(float ratio)
    {
        if (ratio == g_ratio)
            return false;
        g_ratio = ratio;
        return true;
    }
}
