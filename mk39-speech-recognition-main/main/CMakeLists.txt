set(srcs
    main.c
    )

set(requires
    hardware_driver
    )

idf_component_register(SRCS ${srcs}
                       REQUIRES ${requires})

component_compile_options(-w)