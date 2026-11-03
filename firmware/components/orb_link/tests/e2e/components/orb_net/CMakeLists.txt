# TEST ONLY: orb_net for the host run. The hub is hub_stub.py on 127.0.0.1.
idf_component_register(SRCS "orb_net_e2e.c"
                       INCLUDE_DIRS "../../../../../orb_net/include")
