AC_ADD_SCRIPT("${CMAKE_CURRENT_LIST_DIR}/src")

AC_ADD_CONFIG_FILE("${CMAKE_CURRENT_LIST_DIR}/conf/mod_dungeon_master.conf.dist")

install(DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/lua_scripts/"
	DESTINATION "${CMAKE_INSTALL_PREFIX}/bin/lua_scripts/")
