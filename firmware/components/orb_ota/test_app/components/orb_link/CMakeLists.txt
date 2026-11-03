# Stand-in for orb_link: the real header, so orb_ota is compiled against the
# contract, and a fake that lets test_main.c play the hub.
idf_component_register(SRCS "orb_link_stub.c"
                       INCLUDE_DIRS "../../../../orb_link/include" ".")
