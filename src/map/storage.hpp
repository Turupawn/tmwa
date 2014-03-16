#ifndef TMWA_MAP_STORAGE_HPP
#define TMWA_MAP_STORAGE_HPP

// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see COPYING in the main folder

# include "map.hpp"

int storage_storageopen(dumb_ptr<map_session_data> sd);
int storage_storageadd(dumb_ptr<map_session_data> sd, int index, int amount);
int storage_storageget(dumb_ptr<map_session_data> sd, int index, int amount);
int storage_storageclose(dumb_ptr<map_session_data> sd);
void do_final_storage(void);
struct storage *account2storage(int account_id);
struct storage *account2storage2(int account_id);
int storage_storage_quit(dumb_ptr<map_session_data> sd);
int storage_storage_save(int account_id, int final);
int storage_storage_saved(int account_id);

#endif // TMWA_MAP_STORAGE_HPP
