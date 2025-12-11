#define REGISTER_CONFIG(Type, Name) \
    static bool reg_##Type = [](){ \
        ConfigRegistry::instance().register_type(Name, \
            [](const nlohmann::json& j){ return std::make_shared<Type>(Type::from_json(j)); }); \
        return true; \
    }();
