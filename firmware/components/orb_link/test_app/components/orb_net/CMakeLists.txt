# Test-only stand-in for orb_net (written in parallel), so orb_link links on
# its own. A project component wins over the real one in firmware/components.
idf_component_register(SRCS "orb_net_stub.c"
                       INCLUDE_DIRS "../../../../orb_net/include")
