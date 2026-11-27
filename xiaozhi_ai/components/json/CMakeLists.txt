# Local cJSON component (IDF v6.1 no longer ships components/json).
# Source: espressif__cjson 1.7.19 (idf-extra-components), copied verbatim.
# Kconfig limits inlined as compile definitions so no Kconfig entry is needed.
idf_component_register(SRCS "cJSON.c"
                            "cJSON_Utils.c"
                    INCLUDE_DIRS ".")

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    CJSON_NESTING_LIMIT=20
    CJSON_CIRCULAR_LIMIT=100)
