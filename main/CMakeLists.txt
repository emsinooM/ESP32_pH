set(COMPONENT_INCLUDE_DIRS
    inc
)

idf_component_register(SRC_DIRS "src"
                    INCLUDE_DIRS "." ${COMPONENT_INCLUDE_DIRS}
                    )


