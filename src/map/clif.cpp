#include "clif.hpp"
//    clif.cpp - Network interface to the client.
//
//    Copyright © ????-2004 Athena Dev Teams
//    Copyright © 2004-2011 The Mana World Development Team
//    Copyright © 2011-2014 Ben Longbons <b.r.longbons@gmail.com>
//
//    This file is part of The Mana World (Athena server)
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include <arpa/inet.h>

#include <cstdlib>
#include <cstring>
#include <ctime>

#include "../compat/alg.hpp"
#include "../compat/attr.hpp"
#include "../compat/fun.hpp"
#include "../compat/nullpo.hpp"

#include "../ints/cmp.hpp"

#include "../strings/astring.hpp"
#include "../strings/zstring.hpp"
#include "../strings/xstring.hpp"

#include "../generic/random.hpp"

#include "../io/cxxstdio.hpp"
#include "../io/write.hpp"

#include "../mmo/md5more.hpp"
#include "../mmo/socket.hpp"
#include "../mmo/timer.hpp"
#include "../mmo/version.hpp"

#include "atcommand.hpp"
#include "battle.hpp"
#include "chrif.hpp"
#include "intif.hpp"
#include "itemdb.hpp"
#include "magic.hpp"
#include "map.hpp"
#include "npc.hpp"
#include "party.hpp"
#include "pc.hpp"
#include "skill.hpp"
#include "storage.hpp"
#include "tmw.hpp"
#include "trade.hpp"

#include "../poison.hpp"

#define DUMP_UNKNOWN_PACKET     1

constexpr int EMOTE_IGNORED = 0x0e;

// functions list. Rate is how many milliseconds are required between
// calls. Packets exceeding this rate will be dropped. flood_rates in
// map.h must be the same length as this table. rate 0 is default
// rate -1 is unlimited

typedef void (*clif_func)(Session *s, dumb_ptr<map_session_data> sd);
struct func_table
{
    interval_t rate;
    int len;
    clif_func func;

    // ctor exists because interval_t must be explicit
    func_table(int r, int l, clif_func f)
    : rate(r), len(l), func(f)
    {}
};

constexpr int VAR = -1;

extern // not really - defined below
func_table clif_parse_func_table[0x0220];


// local define
enum class SendWho
{
    ALL_CLIENT,
    ALL_SAMEMAP,
    AREA,
    AREA_WOS,
    AREA_CHAT_WOC,
    PARTY,
    PARTY_WOS,
    PARTY_SAMEMAP,
    PARTY_SAMEMAP_WOS,
    PARTY_AREA,
    PARTY_AREA_WOS,
    SELF,
};

inline
void WBUFPOS(uint8_t *p, size_t pos, uint16_t x, uint16_t y)
{
    p += pos;
    p[0] = x >> 2;
    p[1] = (x << 6) | ((y >> 4) & 0x3f);
    p[2] = y << 4;
}
inline
void WBUFPOS2(uint8_t *p, size_t pos, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    p += pos;
    p[0] = x0 >> 2;
    p[1] = (x0 << 6) | ((y0 >> 4) & 0x3f);
    p[2] = (y0 << 4) | ((x1 >> 6) & 0x0f);
    p[3] = (x1 << 2) | ((y1 >> 8) & 0x03);
    p[4] = y1;
}

inline
void WFIFOPOS(Session *s, size_t pos, uint16_t x, uint16_t y)
{
    WBUFPOS(static_cast<uint8_t *>(WFIFOP(s, pos)), 0, x, y);
}
inline
void WFIFOPOS2(Session *s, size_t pos, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    WBUFPOS2(static_cast<uint8_t *>(WFIFOP(s, pos)), 0, x0, y0, x1, y1);
}

static
IP4Address map_ip;
static
int map_port = 5121;

static
int clif_changelook_towards(dumb_ptr<block_list> bl, LOOK type, int val,
                             dumb_ptr<map_session_data> dstsd);
static
void clif_quitsave(Session *, dumb_ptr<map_session_data> sd);

static
void clif_delete(Session *s)
{
    assert (s != char_session);

    dumb_ptr<map_session_data> sd = dumb_ptr<map_session_data>(static_cast<map_session_data *>(s->session_data.get()));
    if (sd && sd->state.auth)
    {
        pc_logout(sd);
        clif_quitsave(s, sd);

        PRINTF("Player [%s] has logged off your server.\n"_fmt, sd->status_key.name);  // Player logout display [Valaris]
    }
    else if (sd)
    {                       // not authentified! (refused by char-server or disconnect before to be authentified)
        PRINTF("Player with account [%d] has logged off your server (not auth account).\n"_fmt, sd->bl_id);    // Player logout display [Yor]
        map_deliddb(sd);  // account_id has been included in the DB before auth answer
    }
}


/*==========================================
 * map鯖のip設定
 *------------------------------------------
 */
void clif_setip(IP4Address ip)
{
    map_ip = ip;
}

/*==========================================
 * map鯖のport設定
 *------------------------------------------
 */
void clif_setport(int port)
{
    map_port = port;
}

/*==========================================
 * map鯖のip読み出し
 *------------------------------------------
 */
IP4Address clif_getip(void)
{
    return map_ip;
}

/*==========================================
 * map鯖のport読み出し
 *------------------------------------------
 */
int clif_getport(void)
{
    return map_port;
}

/*==========================================
 *
 *------------------------------------------
 */
int clif_countusers(void)
{
    int users = 0;

    for (io::FD i : iter_fds())
    {
        Session *s = get_session(i);
        if (!s)
            continue;
        dumb_ptr<map_session_data> sd = dumb_ptr<map_session_data>(static_cast<map_session_data *>(s->session_data.get()));
        if (sd && sd->state.auth && !(battle_config.hide_GM_session && pc_isGM(sd)))
            users++;
    }
    return users;
}

/*==========================================
 * 全てのclientに対してfunc()実行
 *------------------------------------------
 */
int clif_foreachclient(std::function<void (dumb_ptr<map_session_data>)> func)
{
    for (io::FD i : iter_fds())
    {
        Session *s = get_session(i);
        if (!s)
            continue;
        dumb_ptr<map_session_data> sd = dumb_ptr<map_session_data>(static_cast<map_session_data *>(s->session_data.get()));
        if (sd && sd->state.auth)
            func(sd);
    }
    return 0;
}

static
int is_deaf(dumb_ptr<block_list> bl)
{
    if (!bl || bl->bl_type != BL::PC)
        return 0;
    dumb_ptr<map_session_data> sd = bl->is_player();
    return sd->special_state.deaf;
}

static
void clif_emotion_towards(dumb_ptr<block_list> bl,
                                  dumb_ptr<block_list> target, int type);


enum class ChatType
{
    Party,
    Whisper,
    Global,
};

static
AString clif_validate_chat(dumb_ptr<map_session_data> sd, ChatType type);

/*==========================================
 * clif_sendでSendWho::AREA*指定時用
 *------------------------------------------
 */
static
void clif_send_sub(dumb_ptr<block_list> bl, const unsigned char *buf, int len,
        dumb_ptr<block_list> src_bl, SendWho type)
{
    nullpo_retv(bl);
    dumb_ptr<map_session_data> sd = bl->is_player();

    switch (type)
    {
        case SendWho::AREA_WOS:
            if (bl && bl == src_bl)
                return;
            break;

        case SendWho::AREA_CHAT_WOC:
            if (is_deaf(bl)
                && !(bl->bl_type == BL::PC
                     && pc_isGM(src_bl->is_player())))
            {
                clif_emotion_towards(src_bl, bl, EMOTE_IGNORED);
                return;
            }
            if (bl && bl == src_bl)
                return;

            break;
    }

    if (sd->sess != NULL)
    {
        {
            if (clif_parse_func_table[RBUFW(buf, 0)].len)
            {
                // packet must exist
                WFIFO_BUF_CLONE(sd->sess, buf, len);
                WFIFOSET(sd->sess, len);
            }
        }
    }
}

/*==========================================
 *
 *------------------------------------------
 */
static
int clif_send(const uint8_t *buf, int len, dumb_ptr<block_list> bl, SendWho type)
{
    struct party *p = NULL;
    int x0 = 0, x1 = 0, y0 = 0, y1 = 0;

    if (type != SendWho::ALL_CLIENT)
    {
        nullpo_ret(bl);

        if (bl->bl_type == BL::PC)
        {
            dumb_ptr<map_session_data> sd2 = bl->is_player();
            if (bool(sd2->status.option & Option::INVISIBILITY))
            {
                // Obscure hidden GMs

                switch (type)
                {
                    case SendWho::AREA:
                        type = SendWho::SELF;
                        break;

                    case SendWho::AREA_WOS:
                        return 1;

                    default:
                        break;
                }
            }
        }
    }

    switch (type)
    {
        case SendWho::ALL_CLIENT:       // 全クライアントに送信
            for (io::FD i : iter_fds())
            {
                Session *s = get_session(i);
                if (!s)
                    continue;
                dumb_ptr<map_session_data> sd = dumb_ptr<map_session_data>(static_cast<map_session_data *>(s->session_data.get()));
                if (sd && sd->state.auth)
                {
                    if (clif_parse_func_table[RBUFW(buf, 0)].len)
                    {
                        // packet must exist
                        WFIFO_BUF_CLONE(s, buf, len);
                        WFIFOSET(s, len);
                    }
                }
            }
            break;
        case SendWho::ALL_SAMEMAP:      // 同じマップの全クライアントに送信
            for (io::FD i : iter_fds())
            {
                Session *s = get_session(i);
                if (!s)
                    continue;
                dumb_ptr<map_session_data> sd = dumb_ptr<map_session_data>(static_cast<map_session_data *>(s->session_data.get()));
                if (sd && sd->state.auth && sd->bl_m == bl->bl_m)
                {
                    if (clif_parse_func_table[RBUFW(buf, 0)].len)
                    {
                        // packet must exist
                        WFIFO_BUF_CLONE(s, buf, len);
                        WFIFOSET(s, len);
                    }
                }
            }
            break;
        case SendWho::AREA:
        case SendWho::AREA_WOS:
            map_foreachinarea(std::bind(clif_send_sub, ph::_1, buf, len, bl, type),
                    bl->bl_m,
                    bl->bl_x - AREA_SIZE, bl->bl_y - AREA_SIZE,
                    bl->bl_x + AREA_SIZE, bl->bl_y + AREA_SIZE,
                    BL::PC);
            break;
        case SendWho::AREA_CHAT_WOC:
            map_foreachinarea(std::bind(clif_send_sub, ph::_1, buf, len, bl, SendWho::AREA_CHAT_WOC),
                    bl->bl_m,
                    bl->bl_x - (AREA_SIZE), bl->bl_y - (AREA_SIZE),
                    bl->bl_x + (AREA_SIZE), bl->bl_y + (AREA_SIZE),
                    BL::PC);
            break;

        case SendWho::PARTY_AREA:       // 同じ画面内の全パーティーメンバに送信
        case SendWho::PARTY_AREA_WOS:   // 自分以外の同じ画面内の全パーティーメンバに送信
            x0 = bl->bl_x - AREA_SIZE;
            y0 = bl->bl_y - AREA_SIZE;
            x1 = bl->bl_x + AREA_SIZE;
            y1 = bl->bl_y + AREA_SIZE;
            FALLTHROUGH;
        case SendWho::PARTY:            // 全パーティーメンバに送信
        case SendWho::PARTY_WOS:        // 自分以外の全パーティーメンバに送信
        case SendWho::PARTY_SAMEMAP:    // 同じマップの全パーティーメンバに送信
        case SendWho::PARTY_SAMEMAP_WOS:    // 自分以外の同じマップの全パーティーメンバに送信
            if (bl->bl_type == BL::PC)
            {
                dumb_ptr<map_session_data> sd = bl->is_player();
                if (sd->partyspy)
                {
                    p = party_search(sd->partyspy);
                }
                else
                {
                    if (sd->status.party_id)
                        p = party_search(sd->status.party_id);
                }
            }
            if (p)
            {
                for (int i = 0; i < MAX_PARTY; i++)
                {
                    dumb_ptr<map_session_data> sd =  dumb_ptr<map_session_data>(p->member[i].sd);
                    if (sd)
                    {
                        if (sd->bl_id == bl->bl_id && (type == SendWho::PARTY_WOS ||
                                                    type == SendWho::PARTY_SAMEMAP_WOS
                                                    || type == SendWho::PARTY_AREA_WOS))
                            continue;
                        if (type != SendWho::PARTY && type != SendWho::PARTY_WOS && bl->bl_m != sd->bl_m)    // マップチェック
                            continue;
                        if ((type == SendWho::PARTY_AREA || type == SendWho::PARTY_AREA_WOS) &&
                            (sd->bl_x < x0 || sd->bl_y < y0 ||
                             sd->bl_x > x1 || sd->bl_y > y1))
                            continue;
                        if (clif_parse_func_table[RBUFW(buf, 0)].len)
                        {
                            // packet must exist
                            WFIFO_BUF_CLONE(sd->sess, buf, len);
                            WFIFOSET(sd->sess, len);
                        }
                    }
                }
                for (io::FD i : iter_fds())
                {
                    Session *s = get_session(i);
                    if (!s)
                        continue;
                    dumb_ptr<map_session_data> sd = dumb_ptr<map_session_data>(static_cast<map_session_data *>(s->session_data.get()));
                    if (sd && sd->state.auth)
                    {
                        if (sd->partyspy == p->party_id)
                        {
                            if (clif_parse_func_table[RBUFW(buf, 0)].len)
                            {
                                // packet must exist
                                WFIFO_BUF_CLONE(sd->sess, buf, len);
                                WFIFOSET(sd->sess, len);
                            }
                        }
                    }
                }
            }
            break;
        case SendWho::SELF:
        {
            dumb_ptr<map_session_data> sd = bl->is_player();
            if (clif_parse_func_table[RBUFW(buf, 0)].len)
            {
                // packet must exist
                WFIFO_BUF_CLONE(sd->sess, buf, len);
                WFIFOSET(sd->sess, len);
            }
        }
            break;

        default:
            if (battle_config.error_log)
                PRINTF("clif_send まだ作ってないよー\n"_fmt);
            return -1;
    }

    return 0;
}

//
// パケット作って送信
//
/*==========================================
 *
 *------------------------------------------
 */
int clif_authok(dumb_ptr<map_session_data> sd)
{
    nullpo_ret(sd);

    if (!sd)
        return 0;

    if (!sd->sess)
        return 0;

    Session *s = sd->sess;

    WFIFOW(s, 0) = 0x73;
    WFIFOL(s, 2) = gettick().time_since_epoch().count();
    WFIFOPOS(s, 6, sd->bl_x, sd->bl_y);
    WFIFOB(s, 9) = 5;
    WFIFOB(s, 10) = 5;
    WFIFOSET(s, clif_parse_func_table[0x73].len);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
int clif_authfail_fd(Session *s, int type)
{
    if (!s)
        return 0;

    WFIFOW(s, 0) = 0x81;
    WFIFOL(s, 2) = type;
    WFIFOSET(s, clif_parse_func_table[0x81].len);

    clif_setwaitclose(s);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
int clif_charselectok(BlockId id)
{
    dumb_ptr<map_session_data> sd;

    if ((sd = map_id2sd(id)) == NULL)
        return 1;

    if (!sd->sess)
        return 1;

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xb3;
    WFIFOB(s, 2) = 1;
    WFIFOSET(s, clif_parse_func_table[0xb3].len);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
static
int clif_set009e(dumb_ptr<flooritem_data> fitem, uint8_t *buf)
{
    nullpo_ret(fitem);

    //009e <ID>.l <name ID>.w <identify flag>.B <X>.w <Y>.w <subX>.B <subY>.B <amount>.w
    WBUFW(buf, 0) = 0x9e;
    WBUFL(buf, 2) = unwrap<BlockId>(fitem->bl_id);
    WBUFW(buf, 6) = unwrap<ItemNameId>(fitem->item_data.nameid);
    WBUFB(buf, 8) = 1; //identify;
    WBUFW(buf, 9) = fitem->bl_x;
    WBUFW(buf, 11) = fitem->bl_y;
    WBUFB(buf, 13) = fitem->subx;
    WBUFB(buf, 14) = fitem->suby;
    WBUFW(buf, 15) = fitem->item_data.amount;

    return clif_parse_func_table[0x9e].len;
}

/*==========================================
 *
 *------------------------------------------
 */
int clif_dropflooritem(dumb_ptr<flooritem_data> fitem)
{
    uint8_t buf[64];

    nullpo_ret(fitem);

    if (!fitem->item_data.nameid)
        return 0;
    clif_set009e(fitem, buf);
    clif_send(buf, clif_parse_func_table[0x9e].len, fitem, SendWho::AREA);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
int clif_clearflooritem(dumb_ptr<flooritem_data> fitem, Session *s)
{
    unsigned char buf[16];

    nullpo_ret(fitem);

    WBUFW(buf, 0) = 0xa1;
    WBUFL(buf, 2) = unwrap<BlockId>(fitem->bl_id);

    if (!s)
    {
        clif_send(buf, clif_parse_func_table[0xa1].len, fitem, SendWho::AREA);
    }
    else
    {
        WFIFO_BUF_CLONE(s, buf, 6);
        WFIFOSET(s, clif_parse_func_table[0xa1].len);
    }

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
int clif_clearchar(dumb_ptr<block_list> bl, BeingRemoveWhy type)
{
    unsigned char buf[16];

    nullpo_ret(bl);

    WBUFW(buf, 0) = 0x80;
    WBUFL(buf, 2) = unwrap<BlockId>(bl->bl_id);
    if (type == BeingRemoveWhy::DISGUISE)
    {
        WBUFB(buf, 6) = static_cast<uint8_t>(BeingRemoveWhy::GONE);
        clif_send(buf, clif_parse_func_table[0x80].len, bl, SendWho::AREA);
    }
    else
    {
        WBUFB(buf, 6) = static_cast<uint8_t>(type);
        clif_send(buf, clif_parse_func_table[0x80].len, bl,
                   type == BeingRemoveWhy::DEAD ? SendWho::AREA : SendWho::AREA_WOS);
    }

    return 0;
}

static
void clif_clearchar_delay_sub(TimerData *, tick_t,
        dumb_ptr<block_list> bl, BeingRemoveWhy type)
{
    clif_clearchar(bl, type);
    MapBlockLock::freeblock(bl);
}

int clif_clearchar_delay(tick_t tick,
        dumb_ptr<block_list> bl, BeingRemoveWhy type)
{
    dumb_ptr<block_list> tmpbl;
    tmpbl.new_();

    // yikes!
    tmpbl->bl_next = bl->bl_next;
    tmpbl->bl_prev = bl->bl_prev;
    tmpbl->bl_id = bl->bl_id;
    tmpbl->bl_m = bl->bl_m;
    tmpbl->bl_x = bl->bl_x;
    tmpbl->bl_y = bl->bl_y;
    tmpbl->bl_type = bl->bl_type;

    Timer(tick,
            std::bind(clif_clearchar_delay_sub, ph::_1, ph::_2,
                tmpbl, type)
    ).detach();

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
void clif_clearchar_id(BlockId id, BeingRemoveWhy type, Session *s)
{
    WFIFOW(s, 0) = 0x80;
    WFIFOL(s, 2) = unwrap<BlockId>(id);
    WFIFOB(s, 6) = static_cast<uint8_t>(type);
    WFIFOSET(s, clif_parse_func_table[0x80].len);
}

/*==========================================
 *
 *------------------------------------------
 */
static
int clif_set0078(dumb_ptr<map_session_data> sd, unsigned char *buf)
{
    nullpo_ret(sd);

    WBUFW(buf, 0) = 0x1d8;
    WBUFL(buf, 2) = unwrap<BlockId>(sd->bl_id);
    WBUFW(buf, 6) = static_cast<uint16_t>(sd->speed.count());
    WBUFW(buf, 8) = static_cast<uint16_t>(sd->opt1);
    WBUFW(buf, 10) = static_cast<uint16_t>(sd->opt2);
    WBUFW(buf, 12) = static_cast<uint16_t>(sd->status.option);
    WBUFW(buf, 14) = unwrap<Species>(sd->status.species);
    WBUFW(buf, 16) = sd->status.hair;

    int widx = sd->equip_index_maybe[EQUIP::WEAPON];
    int sidx = sd->equip_index_maybe[EQUIP::SHIELD];
    if (sd->attack_spell_override)
        WBUFW(buf, 18) = sd->attack_spell_look_override;
    else
    {
        if (widx >= 0 && sd->inventory_data[widx])
        {
            WBUFW(buf, 18) = unwrap<ItemNameId>(sd->status.inventory[widx].nameid);
        }
        else
            WBUFW(buf, 18) = unwrap<ItemNameId>(ItemNameId());
    }
    if (sidx >= 0 && sidx != widx && sd->inventory_data[sidx])
    {
        WBUFW(buf, 20) = unwrap<ItemNameId>(sd->status.inventory[sidx].nameid);
    }
    else
        WBUFW(buf, 20) = unwrap<ItemNameId>(ItemNameId());
    WBUFW(buf, 22) = unwrap<ItemNameId>(sd->status.head_bottom);
    WBUFW(buf, 24) = unwrap<ItemNameId>(sd->status.head_top);
    WBUFW(buf, 26) = unwrap<ItemNameId>(sd->status.head_mid);
    WBUFW(buf, 28) = sd->status.hair_color;
    WBUFW(buf, 30) = sd->status.clothes_color;
    WBUFW(buf, 32) = static_cast<uint8_t>(sd->head_dir);
    WBUFL(buf, 34) = 0 /*guild_id*/;
    WBUFW(buf, 38) = 0 /*guild_emblem_id*/;
    WBUFW(buf, 40) = sd->status.manner;
    WBUFW(buf, 42) = static_cast<uint16_t>(sd->opt3);
    WBUFB(buf, 44) = sd->status.karma;
    WBUFB(buf, 45) = static_cast<uint8_t>(sd->sex);
    WBUFPOS(buf, 46, sd->bl_x, sd->bl_y);
    // work around ICE in gcc 4.6
    uint8_t dir = static_cast<uint8_t>(sd->dir);
    WBUFB(buf, 48) |= dir;
    WBUFW(buf, 49) = pc_isGM(sd).get_public_word();
    WBUFB(buf, 51) = sd->state.dead_sit;
    WBUFW(buf, 52) = 0;

    return clif_parse_func_table[0x1d8].len;
}

/*==========================================
 *
 *------------------------------------------
 */
static
int clif_set007b(dumb_ptr<map_session_data> sd, unsigned char *buf)
{
    nullpo_ret(sd);

    WBUFW(buf, 0) = 0x1da;
    WBUFL(buf, 2) = unwrap<BlockId>(sd->bl_id);
    WBUFW(buf, 6) = static_cast<uint16_t>(sd->speed.count());
    WBUFW(buf, 8) = static_cast<uint16_t>(sd->opt1);
    WBUFW(buf, 10) = static_cast<uint16_t>(sd->opt2);
    WBUFW(buf, 12) = static_cast<uint16_t>(sd->status.option);
    WBUFW(buf, 14) = unwrap<Species>(sd->status.species);
    WBUFW(buf, 16) = sd->status.hair;
    int widx = sd->equip_index_maybe[EQUIP::WEAPON];
    int sidx = sd->equip_index_maybe[EQUIP::SHIELD];
    if (widx >= 0 && sd->inventory_data[widx])
    {
        WBUFW(buf, 18) = unwrap<ItemNameId>(sd->status.inventory[widx].nameid);
    }
    else
        WBUFW(buf, 18) = unwrap<ItemNameId>(ItemNameId());
    if (sidx >= 0 && sidx != widx && sd->inventory_data[sidx])
    {
        WBUFW(buf, 20) = unwrap<ItemNameId>(sd->status.inventory[sidx].nameid);
    }
    else
        WBUFW(buf, 20) = unwrap<ItemNameId>(ItemNameId());
    WBUFW(buf, 22) = unwrap<ItemNameId>(sd->status.head_bottom);
    WBUFL(buf, 24) = gettick().time_since_epoch().count();
    WBUFW(buf, 28) = unwrap<ItemNameId>(sd->status.head_top);
    WBUFW(buf, 30) = unwrap<ItemNameId>(sd->status.head_mid);
    WBUFW(buf, 32) = sd->status.hair_color;
    WBUFW(buf, 34) = sd->status.clothes_color;
    WBUFW(buf, 36) = static_cast<uint8_t>(sd->head_dir);
    WBUFL(buf, 38) = 0/*guild_id*/;
    WBUFW(buf, 42) = 0/*guild_emblem_id*/;
    WBUFW(buf, 44) = sd->status.manner;
    WBUFW(buf, 46) = static_cast<uint16_t>(sd->opt3);
    WBUFB(buf, 48) = sd->status.karma;
    WBUFB(buf, 49) = static_cast<uint8_t>(sd->sex);
    WBUFPOS2(buf, 50, sd->bl_x, sd->bl_y, sd->to_x, sd->to_y);
    WBUFW(buf, 55) = pc_isGM(sd).get_public_word();
    WBUFB(buf, 57) = 5;
    WBUFW(buf, 58) = 0;

    return clif_parse_func_table[0x1da].len;
}

/*==========================================
 * MOB表示1
 *------------------------------------------
 */
static
int clif_mob0078(dumb_ptr<mob_data> md, unsigned char *buf)
{
    really_memset0(buf, clif_parse_func_table[0x78].len);

    nullpo_ret(md);

    WBUFW(buf, 0) = 0x78;
    WBUFL(buf, 2) = unwrap<BlockId>(md->bl_id);
    WBUFW(buf, 6) = static_cast<uint16_t>(battle_get_speed(md).count());
    WBUFW(buf, 8) = static_cast<uint16_t>(md->opt1);
    WBUFW(buf, 10) = static_cast<uint16_t>(md->opt2);
    WBUFW(buf, 12) = static_cast<uint16_t>(md->option);
    WBUFW(buf, 14) = unwrap<Species>(md->mob_class);
    // snip: stuff do do with disguise as a PC
    WBUFPOS(buf, 46, md->bl_x, md->bl_y);
    // work around ICE in gcc 4.6
    uint8_t dir = static_cast<uint8_t>(md->dir);
    WBUFB(buf, 48) |= dir;
    WBUFB(buf, 49) = 5;
    WBUFB(buf, 50) = 5;
    int level = battle_get_lv(md);
    WBUFW(buf, 52) = (level > battle_config.max_lv) ? battle_config.max_lv : level;

    return clif_parse_func_table[0x78].len;
}

/*==========================================
 * MOB表示2
 *------------------------------------------
 */
static
int clif_mob007b(dumb_ptr<mob_data> md, unsigned char *buf)
{
    really_memset0(buf, clif_parse_func_table[0x7b].len);

    nullpo_ret(md);

    WBUFW(buf, 0) = 0x7b;
    WBUFL(buf, 2) = unwrap<BlockId>(md->bl_id);
    WBUFW(buf, 6) = static_cast<uint16_t>(battle_get_speed(md).count());
    WBUFW(buf, 8) = static_cast<uint16_t>(md->opt1);
    WBUFW(buf, 10) = static_cast<uint16_t>(md->opt2);
    WBUFW(buf, 12) = static_cast<uint16_t>(md->option);
    WBUFW(buf, 14) = unwrap<Species>(md->mob_class);
    // snip: stuff for monsters disguised as PCs
    WBUFL(buf, 22) = gettick().time_since_epoch().count();

    WBUFPOS2(buf, 50, md->bl_x, md->bl_y, md->to_x, md->to_y);
    WBUFB(buf, 56) = 5;
    WBUFB(buf, 57) = 5;
    int level = battle_get_lv(md);
    WBUFW(buf, 58) = (level > battle_config.max_lv) ? battle_config.max_lv : level;

    return clif_parse_func_table[0x7b].len;
}

/*==========================================
 *
 *------------------------------------------
 */
static
int clif_npc0078(dumb_ptr<npc_data> nd, unsigned char *buf)
{
    nullpo_ret(nd);

    really_memset0(buf, clif_parse_func_table[0x78].len);

    WBUFW(buf, 0) = 0x78;
    WBUFL(buf, 2) = unwrap<BlockId>(nd->bl_id);
    WBUFW(buf, 6) = static_cast<uint16_t>(nd->speed.count());
    WBUFW(buf, 14) = nd->npc_class;
    WBUFPOS(buf, 46, nd->bl_x, nd->bl_y);
    // work around ICE in gcc 4.6
    uint8_t dir = static_cast<uint8_t>(nd->dir);
    WBUFB(buf, 48) |= dir;
    WBUFB(buf, 49) = 5;
    WBUFB(buf, 50) = 5;

    return clif_parse_func_table[0x78].len;
}

/* These indices are derived from equip_pos in pc.c and some guesswork */
static
earray<EQUIP, LOOK, LOOK::COUNT> equip_points //=
{{
    EQUIP::NONE,    // base
    EQUIP::NONE,    // hair
    EQUIP::WEAPON,  // weapon
    EQUIP::LEGS,    // head botom -- leg armour
    EQUIP::HAT,     // head top -- hat
    EQUIP::TORSO,   // head mid -- torso armour
    EQUIP::NONE,    // hair colour
    EQUIP::NONE,    // clothes colour
    EQUIP::SHIELD,  // shield
    EQUIP::SHOES,   // shoes
    EQUIP::GLOVES,  // gloves
    EQUIP::CAPE,    // cape
    EQUIP::MISC1,   // misc1
    EQUIP::MISC2,   // misc2
}};

/*==========================================
 *
 *------------------------------------------
 */
int clif_spawnpc(dumb_ptr<map_session_data> sd)
{
    unsigned char buf[128];

    nullpo_ret(sd);

    clif_set0078(sd, buf);

    WBUFW(buf, 0) = 0x1d9;
    WBUFW(buf, 51) = 0;
    clif_send(buf, clif_parse_func_table[0x1d9].len, sd, SendWho::AREA_WOS);

    if (sd->bl_m->flag.get(MapFlag::SNOW))
        clif_specialeffect(sd, 162, 1);
    if (sd->bl_m->flag.get(MapFlag::FOG))
        clif_specialeffect(sd, 233, 1);
    if (sd->bl_m->flag.get(MapFlag::SAKURA))
        clif_specialeffect(sd, 163, 1);
    if (sd->bl_m->flag.get(MapFlag::LEAVES))
        clif_specialeffect(sd, 333, 1);
    if (sd->bl_m->flag.get(MapFlag::RAIN))
        clif_specialeffect(sd, 161, 1);

//        clif_changelook_accessories(sd, NULL);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
int clif_spawnnpc(dumb_ptr<npc_data> nd)
{
    unsigned char buf[64];
    int len;

    nullpo_ret(nd);

    if (nd->npc_class < 0 || nd->flag & 1 || nd->npc_class == INVISIBLE_CLASS)
        return 0;

    really_memset0(buf, clif_parse_func_table[0x7c].len);

    WBUFW(buf, 0) = 0x7c;
    WBUFL(buf, 2) = unwrap<BlockId>(nd->bl_id);
    WBUFW(buf, 6) = static_cast<uint16_t>(nd->speed.count());
    WBUFW(buf, 20) = nd->npc_class;
    WBUFPOS(buf, 36, nd->bl_x, nd->bl_y);

    clif_send(buf, clif_parse_func_table[0x7c].len, nd, SendWho::AREA);

    len = clif_npc0078(nd, buf);
    clif_send(buf, len, nd, SendWho::AREA);

    return 0;
}

int clif_spawn_fake_npc_for_player(dumb_ptr<map_session_data> sd, BlockId fake_npc_id)
{
    nullpo_ret(sd);

    Session *s = sd->sess;

    if (!s)
        return 0;

    WFIFOW(s, 0) = 0x7c;
    WFIFOL(s, 2) = unwrap<BlockId>(fake_npc_id);
    WFIFOW(s, 6) = 0;
    WFIFOW(s, 8) = 0;
    WFIFOW(s, 10) = 0;
    WFIFOW(s, 12) = 0;
    WFIFOW(s, 20) = 127;
    WFIFOPOS(s, 36, sd->bl_x, sd->bl_y);
    WFIFOSET(s, clif_parse_func_table[0x7c].len);

    WFIFOW(s, 0) = 0x78;
    WFIFOL(s, 2) = unwrap<BlockId>(fake_npc_id);
    WFIFOW(s, 6) = 0;
    WFIFOW(s, 8) = 0;
    WFIFOW(s, 10) = 0;
    WFIFOW(s, 12) = 0;
    WFIFOW(s, 14) = 127;      // identifies as NPC
    WFIFOW(s, 20) = 127;
    WFIFOPOS(s, 46, sd->bl_x, sd->bl_y);
    WFIFOPOS(s, 36, sd->bl_x, sd->bl_y);
    WFIFOB(s, 49) = 5;
    WFIFOB(s, 50) = 5;
    WFIFOSET(s, clif_parse_func_table[0x78].len);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
int clif_spawnmob(dumb_ptr<mob_data> md)
{
    unsigned char buf[64];
    int len;

    nullpo_ret(md);

    {
        really_memset0(buf, clif_parse_func_table[0x7c].len);

        WBUFW(buf, 0) = 0x7c;
        WBUFL(buf, 2) = unwrap<BlockId>(md->bl_id);
        WBUFW(buf, 6) = md->stats[mob_stat::SPEED];
        WBUFW(buf, 8) = static_cast<uint16_t>(md->opt1);
        WBUFW(buf, 10) = static_cast<uint16_t>(md->opt2);
        WBUFW(buf, 12) = static_cast<uint16_t>(md->option);
        WBUFW(buf, 20) = unwrap<Species>(md->mob_class);
        WBUFPOS(buf, 36, md->bl_x, md->bl_y);
        clif_send(buf, clif_parse_func_table[0x7c].len, md, SendWho::AREA);
    }

    len = clif_mob0078(md, buf);
    clif_send(buf, len, md, SendWho::AREA);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
static
int clif_servertick(dumb_ptr<map_session_data> sd)
{
    nullpo_ret(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0x7f;
    WFIFOL(s, 2) = sd->server_tick.time_since_epoch().count();
    WFIFOSET(s, clif_parse_func_table[0x7f].len);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
int clif_walkok(dumb_ptr<map_session_data> sd)
{
    nullpo_ret(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0x87;
    WFIFOL(s, 2) = gettick().time_since_epoch().count();
    WFIFOPOS2(s, 6, sd->bl_x, sd->bl_y, sd->to_x, sd->to_y);
    WFIFOB(s, 11) = 0;
    WFIFOSET(s, clif_parse_func_table[0x87].len);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
int clif_movechar(dumb_ptr<map_session_data> sd)
{
    int len;
    unsigned char buf[256];

    nullpo_ret(sd);

    len = clif_set007b(sd, buf);

    clif_send(buf, len, sd, SendWho::AREA_WOS);

    if (battle_config.save_clothcolor == 1 && sd->status.clothes_color > 0)
        clif_changelook(sd, LOOK::CLOTHES_COLOR,
                         sd->status.clothes_color);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_quitsave(Session *, dumb_ptr<map_session_data> sd)
{
    map_quit(sd);
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_waitclose(TimerData *, tick_t, Session *s)
{
    if (s)
        s->set_eof();
}

/*==========================================
 *
 *------------------------------------------
 */
void clif_setwaitclose(Session *s)
{
    s->timed_close = Timer(gettick() + std::chrono::seconds(5),
            std::bind(clif_waitclose, ph::_1, ph::_2,
                s)
    );
}

/*==========================================
 *
 *------------------------------------------
 */
void clif_changemap(dumb_ptr<map_session_data> sd, MapName mapname, int x, int y)
{
    nullpo_retv(sd);

    Session *s = sd->sess;

    WFIFOW(s, 0) = 0x91;
    WFIFO_STRING(s, 2, mapname, 16);
    WFIFOW(s, 18) = x;
    WFIFOW(s, 20) = y;
    WFIFOSET(s, clif_parse_func_table[0x91].len);
}

/*==========================================
 *
 *------------------------------------------
 */
void clif_changemapserver(dumb_ptr<map_session_data> sd,
        MapName mapname, int x, int y, IP4Address ip, int port)
{
    nullpo_retv(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0x92;
    WFIFO_STRING(s, 2, mapname, 16);
    WFIFOW(s, 18) = x;
    WFIFOW(s, 20) = y;
    WFIFOIP(s, 22) = ip;
    WFIFOW(s, 26) = port;
    WFIFOSET(s, clif_parse_func_table[0x92].len);
}

/*==========================================
 *
 *------------------------------------------
 */
void clif_fixpos(dumb_ptr<block_list> bl)
{
    uint8_t buf[16];

    nullpo_retv(bl);

    WBUFW(buf, 0) = 0x88;
    WBUFL(buf, 2) = unwrap<BlockId>(bl->bl_id);
    WBUFW(buf, 6) = bl->bl_x;
    WBUFW(buf, 8) = bl->bl_y;

    clif_send(buf, clif_parse_func_table[0x88].len, bl, SendWho::AREA);
}

/*==========================================
 *
 *------------------------------------------
 */
int clif_npcbuysell(dumb_ptr<map_session_data> sd, BlockId id)
{
    nullpo_ret(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xc4;
    WFIFOL(s, 2) = unwrap<BlockId>(id);
    WFIFOSET(s, clif_parse_func_table[0xc4].len);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
int clif_buylist(dumb_ptr<map_session_data> sd, dumb_ptr<npc_data_shop> nd)
{
    struct item_data *id;
    int i, val;

    nullpo_ret(sd);
    nullpo_ret(nd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xc6;
    for (i = 0; i < nd->shop_items.size(); i++)
    {
        id = itemdb_search(nd->shop_items[i].nameid);
        val = nd->shop_items[i].value;
        WFIFOL(s, 4 + i * 11) = val; // base price
        WFIFOL(s, 8 + i * 11) = val; // actual price
        WFIFOB(s, 12 + i * 11) = static_cast<uint8_t>(id->type);
        WFIFOW(s, 13 + i * 11) = unwrap<ItemNameId>(nd->shop_items[i].nameid);
    }
    WFIFOW(s, 2) = i * 11 + 4;
    WFIFOSET(s, WFIFOW(s, 2));

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
int clif_selllist(dumb_ptr<map_session_data> sd)
{
    int i, c = 0, val;

    nullpo_ret(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xc7;
    for (i = 0; i < MAX_INVENTORY; i++)
    {
        if (sd->status.inventory[i].nameid && sd->inventory_data[i])
        {
            val = sd->inventory_data[i]->value_sell;
            if (val < 0)
                continue;
            WFIFOW(s, 4 + c * 10) = i + 2;
            WFIFOL(s, 6 + c * 10) = val; // base price
            WFIFOL(s, 10 + c * 10) = val; // actual price
            c++;
        }
    }
    WFIFOW(s, 2) = c * 10 + 4;
    WFIFOSET(s, WFIFOW(s, 2));

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
void clif_scriptmes(dumb_ptr<map_session_data> sd, BlockId npcid, XString mes)
{
    nullpo_retv(sd);

    Session *s = sd->sess;

    size_t len = mes.size() + 1;
    WFIFOW(s, 0) = 0xb4;
    WFIFOW(s, 2) = len + 8;
    WFIFOL(s, 4) = unwrap<BlockId>(npcid);
    WFIFO_STRING(s, 8, mes, len);
    WFIFOSET(s, WFIFOW(s, 2));
}

/*==========================================
 *
 *------------------------------------------
 */
void clif_scriptnext(dumb_ptr<map_session_data> sd, BlockId npcid)
{
    nullpo_retv(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xb5;
    WFIFOL(s, 2) = unwrap<BlockId>(npcid);
    WFIFOSET(s, clif_parse_func_table[0xb5].len);
}

/*==========================================
 *
 *------------------------------------------
 */
void clif_scriptclose(dumb_ptr<map_session_data> sd, BlockId npcid)
{
    nullpo_retv(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xb6;
    WFIFOL(s, 2) = unwrap<BlockId>(npcid);
    WFIFOSET(s, clif_parse_func_table[0xb6].len);
}

/*==========================================
 *
 *------------------------------------------
 */
void clif_scriptmenu(dumb_ptr<map_session_data> sd, BlockId npcid, XString mes)
{
    nullpo_retv(sd);

    Session *s = sd->sess;
    size_t len = mes.size() + 1;
    WFIFOW(s, 0) = 0xb7;
    WFIFOW(s, 2) = len + 8;
    WFIFOL(s, 4) = unwrap<BlockId>(npcid);
    WFIFO_STRING(s, 8, mes, len);
    WFIFOSET(s, WFIFOW(s, 2));
}

/*==========================================
 *
 *------------------------------------------
 */
void clif_scriptinput(dumb_ptr<map_session_data> sd, BlockId npcid)
{
    nullpo_retv(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0x142;
    WFIFOL(s, 2) = unwrap<BlockId>(npcid);
    WFIFOSET(s, clif_parse_func_table[0x142].len);
}

/*==========================================
 *
 *------------------------------------------
 */
void clif_scriptinputstr(dumb_ptr<map_session_data> sd, BlockId npcid)
{
    nullpo_retv(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0x1d4;
    WFIFOL(s, 2) = unwrap<BlockId>(npcid);
    WFIFOSET(s, clif_parse_func_table[0x1d4].len);
}

/*==========================================
 *
 *------------------------------------------
 */
int clif_additem(dumb_ptr<map_session_data> sd, int n, int amount, PickupFail fail)
{
    nullpo_ret(sd);

    Session *s = sd->sess;
    if (fail != PickupFail::OKAY)
    {
        WFIFOW(s, 0) = 0xa0;
        WFIFOW(s, 2) = n + 2;
        WFIFOW(s, 4) = amount;
        WFIFOW(s, 6) = 0;
        WFIFOB(s, 8) = 0;
        WFIFOB(s, 9) = 0;
        WFIFOB(s, 10) = 0;
        WFIFOW(s, 11) = 0;
        WFIFOW(s, 13) = 0;
        WFIFOW(s, 15) = 0;
        WFIFOW(s, 17) = 0;
        WFIFOW(s, 19) = 0;
        WFIFOB(s, 21) = 0;
        WFIFOB(s, 22) = static_cast<uint8_t>(fail);
    }
    else
    {
        if (n < 0 || n >= MAX_INVENTORY || !sd->status.inventory[n].nameid
            || sd->inventory_data[n] == NULL)
            return 1;

        WFIFOW(s, 0) = 0xa0;
        WFIFOW(s, 2) = n + 2;
        WFIFOW(s, 4) = amount;
        WFIFOW(s, 6) = unwrap<ItemNameId>(sd->status.inventory[n].nameid);
        WFIFOB(s, 8) = 1; //identify;
        WFIFOB(s, 9) = 0; // broken or attribute;
        WFIFOB(s, 10) = 0; //refine;
        {
            WFIFOW(s, 11) = 0; //card[0];
            WFIFOW(s, 13) = 0; //card[1];
            WFIFOW(s, 15) = 0; //card[2];
            WFIFOW(s, 17) = 0; //card[3];
        }
        WFIFOW(s, 19) = static_cast<uint16_t>(pc_equippoint(sd, n));
        WFIFOB(s, 21) = static_cast<uint8_t>(sd->inventory_data[n]->type == ItemType::_7
            ? ItemType::WEAPON
            : sd->inventory_data[n]->type);
        WFIFOB(s, 22) = static_cast<uint8_t>(fail);
    }

    WFIFOSET(s, clif_parse_func_table[0xa0].len);
    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
void clif_delitem(dumb_ptr<map_session_data> sd, int n, int amount)
{
    nullpo_retv(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xaf;
    WFIFOW(s, 2) = n + 2;
    WFIFOW(s, 4) = amount;

    WFIFOSET(s, clif_parse_func_table[0xaf].len);
}

/*==========================================
 *
 *------------------------------------------
 */
void clif_itemlist(dumb_ptr<map_session_data> sd)
{
    nullpo_retv(sd);

    int n = 0;
    int arrow = -1;
    Session *s = sd->sess;
    WFIFOW(s, 0) = 0x1ee;
    for (int i = 0; i < MAX_INVENTORY; i++)
    {
        if (!sd->status.inventory[i].nameid
            || sd->inventory_data[i] == NULL
            || itemdb_isequip2(sd->inventory_data[i]))
            continue;
        WFIFOW(s, n * 18 + 4) = i + 2;
        WFIFOW(s, n * 18 + 6) = unwrap<ItemNameId>(sd->status.inventory[i].nameid);
        WFIFOB(s, n * 18 + 8) = static_cast<uint8_t>(sd->inventory_data[i]->type);
        WFIFOB(s, n * 18 + 9) = 1; //identify;
        WFIFOW(s, n * 18 + 10) = sd->status.inventory[i].amount;
        if (sd->inventory_data[i]->equip == EPOS::ARROW)
        {
            WFIFOW(s, n * 18 + 12) = static_cast<uint16_t>(EPOS::ARROW);
            if (bool(sd->status.inventory[i].equip))
                arrow = i;      // ついでに矢装備チェック
        }
        else
            WFIFOW(s, n * 18 + 12) = static_cast<uint16_t>(EPOS::ZERO);
        WFIFOW(s, n * 18 + 14) = 0; //card[0];
        WFIFOW(s, n * 18 + 16) = 0; //card[1];
        WFIFOW(s, n * 18 + 18) = 0; //card[2];
        WFIFOW(s, n * 18 + 20) = 0; //card[3];
        n++;
    }
    if (n)
    {
        WFIFOW(s, 2) = 4 + n * 18;
        WFIFOSET(s, WFIFOW(s, 2));
    }
    if (arrow >= 0)
        clif_arrowequip(sd, arrow);
}

/*==========================================
 *
 *------------------------------------------
 */
void clif_equiplist(dumb_ptr<map_session_data> sd)
{
    nullpo_retv(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xa4;
    int n = 0;
    for (int i = 0; i < MAX_INVENTORY; i++)
    {
        if (!sd->status.inventory[i].nameid
            || sd->inventory_data[i] == NULL
            || !itemdb_isequip2(sd->inventory_data[i]))
            continue;
        WFIFOW(s, n * 20 + 4) = i + 2;
        WFIFOW(s, n * 20 + 6) = unwrap<ItemNameId>(sd->status.inventory[i].nameid);
        WFIFOB(s, n * 20 + 8) = static_cast<uint8_t>(
                sd->inventory_data[i]->type == ItemType::_7
                ? ItemType::WEAPON
                : sd->inventory_data[i]->type);
        WFIFOB(s, n * 20 + 9) = 0; //identify;
        WFIFOW(s, n * 20 + 10) = static_cast<uint16_t>(pc_equippoint(sd, i));
        WFIFOW(s, n * 20 + 12) = static_cast<uint16_t>(sd->status.inventory[i].equip);
        WFIFOB(s, n * 20 + 14) = 0; //broken or attribute;
        WFIFOB(s, n * 20 + 15) = 0; //refine;
        {
            WFIFOW(s, n * 20 + 16) = 0; //card[0];
            WFIFOW(s, n * 20 + 18) = 0; //card[1];
            WFIFOW(s, n * 20 + 20) = 0; //card[2];
            WFIFOW(s, n * 20 + 22) = 0; //card[3];
        }
        n++;
    }
    if (n)
    {
        WFIFOW(s, 2) = 4 + n * 20;
        WFIFOSET(s, WFIFOW(s, 2));
    }
}

/*==========================================
 * カプラさんに預けてある消耗品&収集品リスト
 *------------------------------------------
 */
int clif_storageitemlist(dumb_ptr<map_session_data> sd, struct storage *stor)
{
    nullpo_ret(sd);
    nullpo_ret(stor);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0x1f0;
    int n = 0;
    for (int i = 0; i < MAX_STORAGE; i++)
    {
        if (!stor->storage_[i].nameid)
            continue;

        struct item_data *id;
        id = itemdb_search(stor->storage_[i].nameid);
        nullpo_ret(id);
        if (itemdb_isequip2(id))
            continue;

        WFIFOW(s, n * 18 + 4) = i + 1;
        WFIFOW(s, n * 18 + 6) = unwrap<ItemNameId>(stor->storage_[i].nameid);
        WFIFOB(s, n * 18 + 8) = static_cast<uint8_t>(id->type);
        WFIFOB(s, n * 18 + 9) = 0; //identify;
        WFIFOW(s, n * 18 + 10) = stor->storage_[i].amount;
        WFIFOW(s, n * 18 + 12) = 0;
        WFIFOW(s, n * 18 + 14) = 0; //card[0];
        WFIFOW(s, n * 18 + 16) = 0; //card[1];
        WFIFOW(s, n * 18 + 18) = 0; //card[2];
        WFIFOW(s, n * 18 + 20) = 0; //card[3];
        n++;
    }
    if (n)
    {
        WFIFOW(s, 2) = 4 + n * 18;
        WFIFOSET(s, WFIFOW(s, 2));
    }
    return 0;
}

/*==========================================
 * カプラさんに預けてある装備リスト
 *------------------------------------------
 */
int clif_storageequiplist(dumb_ptr<map_session_data> sd, struct storage *stor)
{
    nullpo_ret(sd);
    nullpo_ret(stor);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xa6;
    int n = 0;
    for (int i = 0; i < MAX_STORAGE; i++)
    {
        if (!stor->storage_[i].nameid)
            continue;

        struct item_data *id;
        id = itemdb_search(stor->storage_[i].nameid);
        nullpo_ret(id);
        if (!itemdb_isequip2(id))
            continue;
        WFIFOW(s, n * 20 + 4) = i + 1;
        WFIFOW(s, n * 20 + 6) = unwrap<ItemNameId>(stor->storage_[i].nameid);
        WFIFOB(s, n * 20 + 8) = static_cast<uint8_t>(id->type);
        WFIFOB(s, n * 20 + 9) = 0; //identify;
        WFIFOW(s, n * 20 + 10) = static_cast<uint16_t>(id->equip);
        WFIFOW(s, n * 20 + 12) = static_cast<uint16_t>(stor->storage_[i].equip);
        WFIFOB(s, n * 20 + 14) = 0; //broken or attribute
        WFIFOB(s, n * 20 + 15) = 0; //refine;
        {
            WFIFOW(s, n * 20 + 16) = 0; //card[0];
            WFIFOW(s, n * 20 + 18) = 0; //card[1];
            WFIFOW(s, n * 20 + 20) = 0; //card[2];
            WFIFOW(s, n * 20 + 22) = 0; //card[3];
        }
        n++;
    }
    if (n)
    {
        WFIFOW(s, 2) = 4 + n * 20;
        WFIFOSET(s, WFIFOW(s, 2));
    }
    return 0;
}

/*==========================================
 * ステータスを送りつける
 * 表示専用数字はこの中で計算して送る
 *------------------------------------------
 */
int clif_updatestatus(dumb_ptr<map_session_data> sd, SP type)
{
    int len = 8;

    nullpo_ret(sd);

    Session *s = sd->sess;

    WFIFOW(s, 0) = 0xb0;
    WFIFOW(s, 2) = static_cast<uint16_t>(type);
    switch (type)
    {
            // 00b0
        case SP::WEIGHT:
            pc_checkweighticon(sd);
            // is this because pc_checkweighticon can send other packets?
            WFIFOW(s, 0) = 0xb0;
            WFIFOW(s, 2) = static_cast<uint16_t>(type);
            WFIFOL(s, 4) = sd->weight;
            break;
        case SP::MAXWEIGHT:
            WFIFOL(s, 4) = sd->max_weight;
            break;
        case SP::SPEED:
            // ...
            WFIFOL(s, 4) = static_cast<uint16_t>(sd->speed.count());
            break;
        case SP::BASELEVEL:
            WFIFOL(s, 4) = sd->status.base_level;
            break;
        case SP::JOBLEVEL:
            WFIFOL(s, 4) = 0;
            break;
        case SP::STATUSPOINT:
            WFIFOL(s, 4) = sd->status.status_point;
            break;
        case SP::SKILLPOINT:
            WFIFOL(s, 4) = sd->status.skill_point;
            break;
        case SP::HIT:
            WFIFOL(s, 4) = sd->hit;
            break;
        case SP::FLEE1:
            WFIFOL(s, 4) = sd->flee;
            break;
        case SP::FLEE2:
            WFIFOL(s, 4) = sd->flee2 / 10;
            break;
        case SP::MAXHP:
            WFIFOL(s, 4) = sd->status.max_hp;
            break;
        case SP::MAXSP:
            WFIFOL(s, 4) = sd->status.max_sp;
            break;
        case SP::HP:
            WFIFOL(s, 4) = sd->status.hp;
            break;
        case SP::SP:
            WFIFOL(s, 4) = sd->status.sp;
            break;
        case SP::ASPD:
            WFIFOL(s, 4) = static_cast<uint16_t>(sd->aspd.count());
            break;
        case SP::ATK1:
            WFIFOL(s, 4) = sd->base_atk + sd->watk;
            break;
        case SP::DEF1:
            WFIFOL(s, 4) = sd->def;
            break;
        case SP::MDEF1:
            WFIFOL(s, 4) = sd->mdef;
            break;
        case SP::ATK2:
            WFIFOL(s, 4) = sd->watk2;
            break;
        case SP::DEF2:
            WFIFOL(s, 4) = sd->def2;
            break;
        case SP::MDEF2:
            WFIFOL(s, 4) = sd->mdef2;
            break;
        case SP::CRITICAL:
            WFIFOL(s, 4) = sd->critical / 10;
            break;
        case SP::MATK1:
            WFIFOL(s, 4) = sd->matk1;
            break;
        case SP::MATK2:
            WFIFOL(s, 4) = sd->matk2;
            break;

        case SP::ZENY:
            trade_verifyzeny(sd);
            WFIFOW(s, 0) = 0xb1;
            if (sd->status.zeny < 0)
                sd->status.zeny = 0;
            WFIFOL(s, 4) = sd->status.zeny;
            break;
        case SP::BASEEXP:
            WFIFOW(s, 0) = 0xb1;
            WFIFOL(s, 4) = sd->status.base_exp;
            break;
        case SP::JOBEXP:
            WFIFOW(s, 0) = 0xb1;
            WFIFOL(s, 4) = sd->status.job_exp;
            break;
        case SP::NEXTBASEEXP:
            WFIFOW(s, 0) = 0xb1;
            WFIFOL(s, 4) = pc_nextbaseexp(sd);
            break;
        case SP::NEXTJOBEXP:
            WFIFOW(s, 0) = 0xb1;
            WFIFOL(s, 4) = pc_nextjobexp(sd);
            break;

            // 00be 終了
        case SP::USTR:
        case SP::UAGI:
        case SP::UVIT:
        case SP::UINT:
        case SP::UDEX:
        case SP::ULUK:
            WFIFOW(s, 0) = 0xbe;
            WFIFOB(s, 4) = pc_need_status_point(sd, usp_to_sp(type));
            len = 5;
            break;

            // 013a 終了
        case SP::ATTACKRANGE:
            WFIFOW(s, 0) = 0x13a;
            WFIFOW(s, 2) = (sd->attack_spell_override)
                ? sd->attack_spell_range : sd->attackrange;
            len = 4;
            break;

            // 0141 終了
        case SP::STR:
        case SP::AGI:
        case SP::VIT:
        case SP::INT:
        case SP::DEX:
        case SP::LUK:
        {
            ATTR attr = sp_to_attr(type);
            WFIFOW(s, 0) = 0x141;
            WFIFOL(s, 2) = static_cast<uint16_t>(type);
            WFIFOL(s, 6) = sd->status.attrs[attr];
            WFIFOL(s, 10) = sd->paramb[attr] + sd->parame[attr];
            len = 14;
        }
            break;

        case SP::GM:
            WFIFOL(s, 4) = pc_isGM(sd).get_all_bits();
            break;

        default:
            if (battle_config.error_log)
                PRINTF("clif_updatestatus : make %d routine\n"_fmt,
                        type);
            return 1;
    }
    WFIFOSET(s, len);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
int clif_changelook(dumb_ptr<block_list> bl, LOOK type, int val)
{
    return clif_changelook_towards(bl, type, val, NULL);
}

int clif_changelook_towards(dumb_ptr<block_list> bl, LOOK type, int val,
                             dumb_ptr<map_session_data> dstsd)
{
    unsigned char buf[32];
    dumb_ptr<map_session_data> sd = NULL;

    nullpo_ret(bl);

    if (bl->bl_type == BL::PC)
        sd = bl->is_player();

    if (sd && bool(sd->status.option & Option::INVISIBILITY))
        return 0;

    if (sd
        && (type == LOOK::WEAPON || type == LOOK::SHIELD || type >= LOOK::SHOES))
    {
        WBUFW(buf, 0) = 0x1d7;
        WBUFL(buf, 2) = unwrap<BlockId>(bl->bl_id);
        if (type >= LOOK::SHOES)
        {
            EQUIP equip_point = equip_points[type];

            WBUFB(buf, 6) = static_cast<uint16_t>(type);
            int idx = sd->equip_index_maybe[equip_point];
            if (idx >= 0 && sd->inventory_data[idx])
            {
                WBUFW(buf, 7) = unwrap<ItemNameId>(sd->status.inventory[idx].nameid);
            }
            else
                WBUFW(buf, 7) = 0;
            WBUFW(buf, 9) = 0;
        }
        else
        {
            WBUFB(buf, 6) = 2;
            int widx = sd->equip_index_maybe[EQUIP::WEAPON];
            int sidx = sd->equip_index_maybe[EQUIP::SHIELD];
            if (sd->attack_spell_override)
                WBUFW(buf, 7) = sd->attack_spell_look_override;
            else
            {
                if (widx >= 0 && sd->inventory_data[widx])
                {
                    WBUFW(buf, 7) = unwrap<ItemNameId>(sd->status.inventory[widx].nameid);
                }
                else
                    WBUFW(buf, 7) = 0;
            }
            if (sidx >= 0 && sidx != widx && sd->inventory_data[sidx])
            {
                WBUFW(buf, 9) = unwrap<ItemNameId>(sd->status.inventory[sidx].nameid);
            }
            else
                WBUFW(buf, 9) = 0;
        }
        if (dstsd)
            clif_send(buf, clif_parse_func_table[0x1d7].len, dstsd, SendWho::SELF);
        else
            clif_send(buf, clif_parse_func_table[0x1d7].len, bl, SendWho::AREA);
    }
    else
    {
        WBUFW(buf, 0) = 0x1d7;
        WBUFL(buf, 2) = unwrap<BlockId>(bl->bl_id);
        WBUFB(buf, 6) = static_cast<uint8_t>(type);
        WBUFW(buf, 7) = val;
        WBUFW(buf, 9) = 0;
        if (dstsd)
            clif_send(buf, clif_parse_func_table[0x1d7].len, dstsd, SendWho::SELF);
        else
            clif_send(buf, clif_parse_func_table[0x1d7].len, bl, SendWho::AREA);
    }
    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
static
int clif_initialstatus(dumb_ptr<map_session_data> sd)
{
    nullpo_ret(sd);

    Session *s = sd->sess;

    WFIFOW(s, 0) = 0xbd;
    WFIFOW(s, 2) = sd->status.status_point;

    WFIFOB(s, 4) = saturate<uint8_t>(sd->status.attrs[ATTR::STR]);
    WFIFOB(s, 5) = pc_need_status_point(sd, SP::STR);
    WFIFOB(s, 6) = saturate<uint8_t>(sd->status.attrs[ATTR::AGI]);
    WFIFOB(s, 7) = pc_need_status_point(sd, SP::AGI);
    WFIFOB(s, 8) = saturate<uint8_t>(sd->status.attrs[ATTR::VIT]);
    WFIFOB(s, 9) = pc_need_status_point(sd, SP::VIT);
    WFIFOB(s, 10) = saturate<uint8_t>(sd->status.attrs[ATTR::INT]);
    WFIFOB(s, 11) = pc_need_status_point(sd, SP::INT);
    WFIFOB(s, 12) = saturate<uint8_t>(sd->status.attrs[ATTR::DEX]);
    WFIFOB(s, 13) = pc_need_status_point(sd, SP::DEX);
    WFIFOB(s, 14) = saturate<uint8_t>(sd->status.attrs[ATTR::LUK]);
    WFIFOB(s, 15) = pc_need_status_point(sd, SP::LUK);

    WFIFOW(s, 16) = sd->base_atk + sd->watk;
    WFIFOW(s, 18) = sd->watk2;    //atk bonus
    WFIFOW(s, 20) = sd->matk1;
    WFIFOW(s, 22) = sd->matk2;
    WFIFOW(s, 24) = sd->def;  // def
    WFIFOW(s, 26) = sd->def2;
    WFIFOW(s, 28) = sd->mdef; // mdef
    WFIFOW(s, 30) = sd->mdef2;
    WFIFOW(s, 32) = sd->hit;
    WFIFOW(s, 34) = sd->flee;
    WFIFOW(s, 36) = sd->flee2 / 10;
    WFIFOW(s, 38) = sd->critical / 10;
    WFIFOW(s, 40) = sd->status.karma;
    WFIFOW(s, 42) = sd->status.manner;

    WFIFOSET(s, clif_parse_func_table[0xbd].len);

    clif_updatestatus(sd, SP::STR);
    clif_updatestatus(sd, SP::AGI);
    clif_updatestatus(sd, SP::VIT);
    clif_updatestatus(sd, SP::INT);
    clif_updatestatus(sd, SP::DEX);
    clif_updatestatus(sd, SP::LUK);

    clif_updatestatus(sd, SP::ATTACKRANGE);
    clif_updatestatus(sd, SP::ASPD);

    return 0;
}

/*==========================================
 *矢装備
 *------------------------------------------
 */
int clif_arrowequip(dumb_ptr<map_session_data> sd, int val)
{
    nullpo_ret(sd);

    sd->attacktarget = BlockId();

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0x013c;
    WFIFOW(s, 2) = val + 2;   //矢のアイテムID

    WFIFOSET(s, clif_parse_func_table[0x013c].len);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
int clif_arrow_fail(dumb_ptr<map_session_data> sd, int type)
{
    nullpo_ret(sd);

    Session *s = sd->sess;

    WFIFOW(s, 0) = 0x013b;
    WFIFOW(s, 2) = type;

    WFIFOSET(s, clif_parse_func_table[0x013b].len);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
int clif_statusupack(dumb_ptr<map_session_data> sd, SP type, int ok, int val)
{
    nullpo_ret(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xbc;
    WFIFOW(s, 2) = static_cast<uint16_t>(type);
    WFIFOB(s, 4) = ok;
    WFIFOB(s, 5) = val;
    WFIFOSET(s, clif_parse_func_table[0xbc].len);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
int clif_equipitemack(dumb_ptr<map_session_data> sd, int n, EPOS pos, int ok)
{
    nullpo_ret(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xaa;
    WFIFOW(s, 2) = n + 2;
    WFIFOW(s, 4) = static_cast<uint16_t>(pos);
    WFIFOB(s, 6) = ok;
    WFIFOSET(s, clif_parse_func_table[0xaa].len);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
int clif_unequipitemack(dumb_ptr<map_session_data> sd, int n, EPOS pos, int ok)
{
    nullpo_ret(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xac;
    WFIFOW(s, 2) = n + 2;
    WFIFOW(s, 4) = static_cast<uint16_t>(pos);
    WFIFOB(s, 6) = ok;
    WFIFOSET(s, clif_parse_func_table[0xac].len);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
int clif_misceffect(dumb_ptr<block_list> bl, int type)
{
    uint8_t buf[32];

    nullpo_ret(bl);

    WBUFW(buf, 0) = 0x19b;
    WBUFL(buf, 2) = unwrap<BlockId>(bl->bl_id);
    WBUFL(buf, 6) = type;

    clif_send(buf, clif_parse_func_table[0x19b].len, bl, SendWho::AREA);

    return 0;
}

/*==========================================
 * 表示オプション変更
 *------------------------------------------
 */
int clif_changeoption(dumb_ptr<block_list> bl)
{
    uint8_t buf[32];
    eptr<struct status_change, StatusChange, StatusChange::MAX_STATUSCHANGE> sc_data;

    nullpo_ret(bl);

    Option option = *battle_get_option(bl);
    sc_data = battle_get_sc_data(bl);

    WBUFW(buf, 0) = 0x119;
    WBUFL(buf, 2) = unwrap<BlockId>(bl->bl_id);
    WBUFW(buf, 6) = static_cast<uint16_t>(*battle_get_opt1(bl));
    WBUFW(buf, 8) = static_cast<uint16_t>(*battle_get_opt2(bl));
    WBUFW(buf, 10) = static_cast<uint16_t>(option);
    WBUFB(buf, 12) = 0;        // ??

    clif_send(buf, clif_parse_func_table[0x119].len, bl, SendWho::AREA);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
int clif_useitemack(dumb_ptr<map_session_data> sd, int index, int amount,
                     int ok)
{
    nullpo_ret(sd);

    if (!ok)
    {
        Session *s = sd->sess;
        WFIFOW(s, 0) = 0xa8;
        WFIFOW(s, 2) = index + 2;
        WFIFOW(s, 4) = amount;
        WFIFOB(s, 6) = ok;
        WFIFOSET(s, clif_parse_func_table[0xa8].len);
    }
    else
    {
        uint8_t buf[32];

        WBUFW(buf, 0) = 0x1c8;
        WBUFW(buf, 2) = index + 2;
        WBUFW(buf, 4) = unwrap<ItemNameId>(sd->status.inventory[index].nameid);
        WBUFL(buf, 6) = unwrap<BlockId>(sd->bl_id);
        WBUFW(buf, 10) = amount;
        WBUFB(buf, 12) = ok;
        clif_send(buf, clif_parse_func_table[0x1c8].len, sd, SendWho::SELF);
    }

    return 0;
}

/*==========================================
 * 取り引き要請受け
 *------------------------------------------
 */
void clif_traderequest(dumb_ptr<map_session_data> sd, CharName name)
{
    nullpo_retv(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xe5;
    WFIFO_STRING(s, 2, name.to__actual(), 24);
    WFIFOSET(s, clif_parse_func_table[0xe5].len);
}

/*==========================================
 * 取り引き要求応答
 *------------------------------------------
 */
void clif_tradestart(dumb_ptr<map_session_data> sd, int type)
{
    nullpo_retv(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xe7;
    WFIFOB(s, 2) = type;
    WFIFOSET(s, clif_parse_func_table[0xe7].len);
}

/*==========================================
 * 相手方からのアイテム追加
 *------------------------------------------
 */
void clif_tradeadditem(dumb_ptr<map_session_data> sd,
        dumb_ptr<map_session_data> tsd, int index, int amount)
{
    nullpo_retv(sd);
    nullpo_retv(tsd);

    Session *s = tsd->sess;
    WFIFOW(s, 0) = 0xe9;
    WFIFOL(s, 2) = amount;
    if (index == 0)
    {
        WFIFOW(s, 6) = 0;     // type id
        WFIFOB(s, 8) = 0;     //identify flag
        WFIFOB(s, 9) = 0;     // attribute
        WFIFOB(s, 10) = 0;    //refine
        WFIFOW(s, 11) = 0;    //card (4w)
        WFIFOW(s, 13) = 0;    //card (4w)
        WFIFOW(s, 15) = 0;    //card (4w)
        WFIFOW(s, 17) = 0;    //card (4w)
    }
    else
    {
        index -= 2;
        WFIFOW(s, 6) = unwrap<ItemNameId>(sd->status.inventory[index].nameid);    // type id
        WFIFOB(s, 8) = 0; //identify;
        WFIFOB(s, 9) = 0; //broken or attribute;
        WFIFOB(s, 10) = 0; //refine;
        {
            WFIFOW(s, 11) = 0; //card[0];
            WFIFOW(s, 13) = 0; //card[1];
            WFIFOW(s, 15) = 0; //card[2];
            WFIFOW(s, 17) = 0; //card[3];
        }
    }
    WFIFOSET(s, clif_parse_func_table[0xe9].len);
}

/*==========================================
 * アイテム追加成功/失敗
 *------------------------------------------
 */
int clif_tradeitemok(dumb_ptr<map_session_data> sd, int index, int amount,
                      int fail)
{
    nullpo_ret(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0x1b1;
    WFIFOW(s, 2) = index;
    WFIFOW(s, 4) = amount;
    WFIFOB(s, 6) = fail;
    WFIFOSET(s, clif_parse_func_table[0x1b1].len);

    return 0;
}

/*==========================================
 * 取り引きok押し
 *------------------------------------------
 */
int clif_tradedeal_lock(dumb_ptr<map_session_data> sd, int fail)
{
    nullpo_ret(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xec;
    WFIFOB(s, 2) = fail;      // 0=you 1=the other person
    WFIFOSET(s, clif_parse_func_table[0xec].len);

    return 0;
}

/*==========================================
 * 取り引きがキャンセルされました
 *------------------------------------------
 */
int clif_tradecancelled(dumb_ptr<map_session_data> sd)
{
    nullpo_ret(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xee;
    WFIFOSET(s, clif_parse_func_table[0xee].len);

    return 0;
}

/*==========================================
 * 取り引き完了
 *------------------------------------------
 */
int clif_tradecompleted(dumb_ptr<map_session_data> sd, int fail)
{
    nullpo_ret(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xf0;
    WFIFOB(s, 2) = fail;
    WFIFOSET(s, clif_parse_func_table[0xf0].len);

    return 0;
}

/*==========================================
 * カプラ倉庫のアイテム数を更新
 *------------------------------------------
 */
int clif_updatestorageamount(dumb_ptr<map_session_data> sd,
                              struct storage *stor)
{
    nullpo_ret(sd);
    nullpo_ret(stor);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xf2;      // update storage amount
    WFIFOW(s, 2) = stor->storage_amount;  //items
    WFIFOW(s, 4) = MAX_STORAGE;   //items max
    WFIFOSET(s, clif_parse_func_table[0xf2].len);

    return 0;
}

/*==========================================
 * カプラ倉庫にアイテムを追加する
 *------------------------------------------
 */
int clif_storageitemadded(dumb_ptr<map_session_data> sd, struct storage *stor,
                           int index, int amount)
{
    nullpo_ret(sd);
    nullpo_ret(stor);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xf4;      // Storage item added
    WFIFOW(s, 2) = index + 1; // index
    WFIFOL(s, 4) = amount;    // amount
/*      if ((view = itemdb_viewid(stor->storage_[index].nameid)) > 0)
                WFIFOW(fd,8) =view;
        else*/
    WFIFOW(s, 8) = unwrap<ItemNameId>(stor->storage_[index].nameid);
    WFIFOB(s, 10) = 0; //identify;
    WFIFOB(s, 11) = 0; //broken or attribute;
    WFIFOB(s, 12) = 0; //refine;
    {
        WFIFOW(s, 13) = 0; //card[0];
        WFIFOW(s, 15) = 0; //card[1];
        WFIFOW(s, 17) = 0; //card[2];
        WFIFOW(s, 19) = 0; //card[3];
    }
    WFIFOSET(s, clif_parse_func_table[0xf4].len);

    return 0;
}

/*==========================================
 * カプラ倉庫からアイテムを取り去る
 *------------------------------------------
 */
int clif_storageitemremoved(dumb_ptr<map_session_data> sd, int index,
                             int amount)
{
    nullpo_ret(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xf6;      // Storage item removed
    WFIFOW(s, 2) = index + 1;
    WFIFOL(s, 4) = amount;
    WFIFOSET(s, clif_parse_func_table[0xf6].len);

    return 0;
}

/*==========================================
 * カプラ倉庫を閉じる
 *------------------------------------------
 */
int clif_storageclose(dumb_ptr<map_session_data> sd)
{
    nullpo_ret(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xf8;      // Storage Closed
    WFIFOSET(s, clif_parse_func_table[0xf8].len);

    return 0;
}

void clif_changelook_accessories(dumb_ptr<block_list> bl,
                             dumb_ptr<map_session_data> dest)
{
    for (LOOK i = LOOK::SHOES; i < LOOK::COUNT; i = LOOK(static_cast<uint8_t>(i) + 1))
        clif_changelook_towards(bl, i, 0, dest);
}

//
// callback系 ?
//
/*==========================================
 * PC表示
 *------------------------------------------
 */
static
void clif_getareachar_pc(dumb_ptr<map_session_data> sd,
                          dumb_ptr<map_session_data> dstsd)
{
    int len;

    if (bool(dstsd->status.option & Option::INVISIBILITY))
        return;

    nullpo_retv(sd);
    nullpo_retv(dstsd);

    uint8_t buf[256];
    if (dstsd->walktimer)
    {
        len = clif_set007b(dstsd, buf);
    }
    else
    {
        len = clif_set0078(dstsd, buf);
    }
    WFIFO_BUF_CLONE(sd->sess, buf, len);
    WFIFOSET(sd->sess, len);

    if (battle_config.save_clothcolor == 1 && dstsd->status.clothes_color > 0)
        clif_changelook(dstsd, LOOK::CLOTHES_COLOR,
                         dstsd->status.clothes_color);

    clif_changelook_accessories(sd, dstsd);
    clif_changelook_accessories(dstsd, sd);
}

/*==========================================
 * NPC表示
 *------------------------------------------
 */
static
void clif_getareachar_npc(dumb_ptr<map_session_data> sd, dumb_ptr<npc_data> nd)
{
    int len;

    nullpo_retv(sd);
    nullpo_retv(nd);

    if (nd->npc_class < 0 || nd->flag & 1 || nd->npc_class == INVISIBLE_CLASS)
        return;

    len = clif_npc0078(nd, static_cast<uint8_t *>(WFIFOP(sd->sess, 0)));
    WFIFOSET(sd->sess, len);
}

/*==========================================
 * 移動停止
 *------------------------------------------
 */
int clif_movemob(dumb_ptr<mob_data> md)
{
    unsigned char buf[256];
    int len;

    nullpo_ret(md);

    len = clif_mob007b(md, buf);
    clif_send(buf, len, md, SendWho::AREA);

    return 0;
}

/*==========================================
 * モンスターの位置修正
 *------------------------------------------
 */
int clif_fixmobpos(dumb_ptr<mob_data> md)
{
    unsigned char buf[256];
    int len;

    nullpo_ret(md);

    if (md->state.state == MS::WALK)
    {
        len = clif_mob007b(md, buf);
        clif_send(buf, len, md, SendWho::AREA);
    }
    else
    {
        len = clif_mob0078(md, buf);
        clif_send(buf, len, md, SendWho::AREA);
    }

    return 0;
}

/*==========================================
 * PCの位置修正
 *------------------------------------------
 */
int clif_fixpcpos(dumb_ptr<map_session_data> sd)
{
    unsigned char buf[256];
    int len;

    nullpo_ret(sd);

    if (sd->walktimer)
    {
        len = clif_set007b(sd, buf);
        clif_send(buf, len, sd, SendWho::AREA);
    }
    else
    {
        len = clif_set0078(sd, buf);
        clif_send(buf, len, sd, SendWho::AREA);
    }
    clif_changelook_accessories(sd, NULL);

    return 0;
}

/*==========================================
 * 通常攻撃エフェクト＆ダメージ
 *------------------------------------------
 */
int clif_damage(dumb_ptr<block_list> src, dumb_ptr<block_list> dst,
        tick_t tick, interval_t sdelay, interval_t ddelay, int damage,
        int div, DamageType type, int damage2)
{
    unsigned char buf[256];
    eptr<struct status_change, StatusChange, StatusChange::MAX_STATUSCHANGE> sc_data;

    nullpo_ret(src);
    nullpo_ret(dst);

    sc_data = battle_get_sc_data(dst);

    WBUFW(buf, 0) = 0x8a;
    WBUFL(buf, 2) = unwrap<BlockId>(src->bl_id);
    WBUFL(buf, 6) = unwrap<BlockId>(dst->bl_id);
    WBUFL(buf, 10) = tick.time_since_epoch().count();
    WBUFL(buf, 14) = sdelay.count();
    WBUFL(buf, 18) = ddelay.count();
    WBUFW(buf, 22) = (damage > 0x7fff) ? 0x7fff : damage;
    WBUFW(buf, 24) = div;
    WBUFB(buf, 26) = static_cast<uint8_t>(type);
    WBUFW(buf, 27) = damage2;
    clif_send(buf, clif_parse_func_table[0x8a].len, src, SendWho::AREA);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_getareachar_mob(dumb_ptr<map_session_data> sd, dumb_ptr<mob_data> md)
{
    int len;
    nullpo_retv(sd);
    nullpo_retv(md);

    if (md->state.state == MS::WALK)
    {
        len = clif_mob007b(md, static_cast<uint8_t *>(WFIFOP(sd->sess, 0)));
        WFIFOSET(sd->sess, len);
    }
    else
    {
        len = clif_mob0078(md, static_cast<uint8_t *>(WFIFOP(sd->sess, 0)));
        WFIFOSET(sd->sess, len);
    }
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_getareachar_item(dumb_ptr<map_session_data> sd,
                            dumb_ptr<flooritem_data> fitem)
{
    nullpo_retv(sd);
    nullpo_retv(fitem);

    Session *s = sd->sess;
    //009d <ID>.l <item ID>.w <identify flag>.B <X>.w <Y>.w <amount>.w <subX>.B <subY>.B
    WFIFOW(s, 0) = 0x9d;
    WFIFOL(s, 2) = unwrap<BlockId>(fitem->bl_id);
    WFIFOW(s, 6) = unwrap<ItemNameId>(fitem->item_data.nameid);
    WFIFOB(s, 8) = 0; //identify;
    WFIFOW(s, 9) = fitem->bl_x;
    WFIFOW(s, 11) = fitem->bl_y;
    WFIFOW(s, 13) = fitem->item_data.amount;
    WFIFOB(s, 15) = fitem->subx;
    WFIFOB(s, 16) = fitem->suby;

    WFIFOSET(s, clif_parse_func_table[0x9d].len);
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_getareachar(dumb_ptr<block_list> bl, dumb_ptr<map_session_data> sd)
{
    nullpo_retv(bl);

    switch (bl->bl_type)
    {
        case BL::PC:
            if (sd == bl->is_player())
                break;
            clif_getareachar_pc(sd, bl->is_player());
            break;
        case BL::NPC:
            clif_getareachar_npc(sd, bl->is_npc());
            break;
        case BL::MOB:
            clif_getareachar_mob(sd, bl->is_mob());
            break;
        case BL::ITEM:
            clif_getareachar_item(sd, bl->is_item());
            break;
        case BL::SPELL:
            // spell objects are not visible
            // (at least, I *think* that's what this code is for)
            // in any case, this is not a behavior change, just silencing
            // the below warning
            break;
        default:
            if (battle_config.error_log)
                PRINTF("get area char ??? %d\n"_fmt,
                        bl->bl_type);
            break;
    }
}

/*==========================================
 *
 *------------------------------------------
 */
void clif_pcoutsight(dumb_ptr<block_list> bl, dumb_ptr<map_session_data> sd)
{
    dumb_ptr<map_session_data> dstsd;

    nullpo_retv(bl);
    nullpo_retv(sd);

    switch (bl->bl_type)
    {
        case BL::PC:
            dstsd = bl->is_player();
            if (sd != dstsd)
            {
                clif_clearchar_id(dstsd->bl_id, BeingRemoveWhy::GONE, sd->sess);
                clif_clearchar_id(sd->bl_id, BeingRemoveWhy::GONE, dstsd->sess);
            }
            break;
        case BL::NPC:
            if (bl->is_npc()->npc_class != INVISIBLE_CLASS)
                clif_clearchar_id(bl->bl_id, BeingRemoveWhy::GONE, sd->sess);
            break;
        case BL::MOB:
            clif_clearchar_id(bl->bl_id, BeingRemoveWhy::GONE, sd->sess);
            break;
        case BL::ITEM:
            clif_clearflooritem(bl->is_item(), sd->sess);
            break;
    }
}

/*==========================================
 *
 *------------------------------------------
 */
void clif_pcinsight(dumb_ptr<block_list> bl, dumb_ptr<map_session_data> sd)
{
    dumb_ptr<map_session_data> dstsd;

    nullpo_retv(bl);
    nullpo_retv(sd);

    switch (bl->bl_type)
    {
        case BL::PC:
            dstsd = bl->is_player();
            if (sd != dstsd)
            {
                clif_getareachar_pc(sd, dstsd);
                clif_getareachar_pc(dstsd, sd);
            }
            break;
        case BL::NPC:
            clif_getareachar_npc(sd, bl->is_npc());
            break;
        case BL::MOB:
            clif_getareachar_mob(sd, bl->is_mob());
            break;
        case BL::ITEM:
            clif_getareachar_item(sd, bl->is_item());
            break;
    }
}

/*==========================================
 *
 *------------------------------------------
 */
void clif_moboutsight(dumb_ptr<block_list> bl, dumb_ptr<mob_data> md)
{
    dumb_ptr<map_session_data> sd;

    nullpo_retv(bl);
    nullpo_retv(md);

    if (bl->bl_type == BL::PC)
    {
        sd = bl->is_player();
        clif_clearchar_id(md->bl_id, BeingRemoveWhy::GONE, sd->sess);
    }
}

/*==========================================
 *
 *------------------------------------------
 */
void clif_mobinsight(dumb_ptr<block_list> bl, dumb_ptr<mob_data> md)
{
    dumb_ptr<map_session_data> sd;

    nullpo_retv(bl);
    nullpo_retv(md);

    if (bl->bl_type == BL::PC)
    {
        sd = bl->is_player();
        clif_getareachar_mob(sd, md);
    }
}

/*==========================================
 *
 *------------------------------------------
 */
int clif_skillinfo(dumb_ptr<map_session_data> sd, SkillID skillid, int type,
                    int range)
{
    nullpo_ret(sd);

    Session *s = sd->sess;
    if (!sd->status.skill[skillid].lv)
        return 0;
    WFIFOW(s, 0) = 0x147;
    WFIFOW(s, 2) = static_cast<uint16_t>(skillid);
    if (type < 0)
        WFIFOW(s, 4) = skill_get_inf(skillid);
    else
        WFIFOW(s, 4) = type;
    WFIFOW(s, 6) = 0;
    WFIFOW(s, 8) = sd->status.skill[skillid].lv;
    WFIFOW(s, 10) = skill_get_sp(skillid, sd->status.skill[skillid].lv);
    if (range < 0)
    {
        range = skill_get_range(skillid, sd->status.skill[skillid].lv);
        if (range < 0)
            range = battle_get_range(sd) - (range + 1);
        WFIFOW(s, 12) = range;
    }
    else
        WFIFOW(s, 12) = range;
    WFIFO_ZERO(s, 14, 24);
    WFIFOB(s, 38) = sd->status.skill[skillid].lv < skill_get_max_raise(skillid);
    WFIFOSET(s, clif_parse_func_table[0x147].len);

    return 0;
}

/*==========================================
 * スキルリストを送信する
 *------------------------------------------
 */
void clif_skillinfoblock(dumb_ptr<map_session_data> sd)
{
    int len = 4, range;

    nullpo_retv(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0x10f;
    for (SkillID i : erange(SkillID(), MAX_SKILL))
    {
        if (sd->status.skill[i].lv && sd->tmw_version >= 1)
        {
            // [Fate] Version 1 and later don't crash because of bad skill IDs anymore
            WFIFOW(s, len) = static_cast<uint16_t>(i);
            WFIFOW(s, len + 2) = skill_get_inf(i);
            WFIFOW(s, len + 4) = static_cast<uint16_t>(
                skill_db[i].poolflags
                | (sd->status.skill[i].flags & SkillFlags::POOL_ACTIVATED));
            WFIFOW(s, len + 6) = sd->status.skill[i].lv;
            WFIFOW(s, len + 8) = skill_get_sp(i, sd->status.skill[i].lv);
            range = skill_get_range(i, sd->status.skill[i].lv);
            if (range < 0)
                range = battle_get_range(sd) - (range + 1);
            WFIFOW(s, len + 10) = range;
            WFIFO_ZERO(s, len + 12, 24);
            WFIFOB(s, len + 36) = sd->status.skill[i].lv < skill_get_max_raise(i);
            len += 37;
        }
    }
    WFIFOW(s, 2) = len;
    WFIFOSET(s, len);
}

/*==========================================
 * スキル割り振り通知
 *------------------------------------------
 */
int clif_skillup(dumb_ptr<map_session_data> sd, SkillID skill_num)
{
    int range;

    nullpo_ret(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0x10e;
    WFIFOW(s, 2) = static_cast<uint16_t>(skill_num);
    WFIFOW(s, 4) = sd->status.skill[skill_num].lv;
    WFIFOW(s, 6) = skill_get_sp(skill_num, sd->status.skill[skill_num].lv);
    range = skill_get_range(skill_num, sd->status.skill[skill_num].lv);
    if (range < 0)
        range = battle_get_range(sd) - (range + 1);
    WFIFOW(s, 8) = range;
    WFIFOB(s, 10) = sd->status.skill[skill_num].lv < skill_get_max_raise(skill_num);
    WFIFOSET(s, clif_parse_func_table[0x10e].len);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
int clif_skillcastcancel(dumb_ptr<block_list> bl)
{
    unsigned char buf[16];

    nullpo_ret(bl);

    WBUFW(buf, 0) = 0x1b9;
    WBUFL(buf, 2) = unwrap<BlockId>(bl->bl_id);
    clif_send(buf, clif_parse_func_table[0x1b9].len, bl, SendWho::AREA);

    return 0;
}

/*==========================================
 * スキル詠唱失敗
 *------------------------------------------
 */
int clif_skill_fail(dumb_ptr<map_session_data> sd, SkillID skill_id, int type,
                     int btype)
{
    nullpo_ret(sd);

    Session *s = sd->sess;

    if (type == 0x4 && battle_config.display_delay_skill_fail == 0)
    {
        return 0;
    }

    WFIFOW(s, 0) = 0x110;
    WFIFOW(s, 2) = static_cast<uint16_t>(skill_id);
    WFIFOW(s, 4) = btype;
    WFIFOW(s, 6) = 0;
    WFIFOB(s, 8) = 0;
    WFIFOB(s, 9) = type;
    WFIFOSET(s, clif_parse_func_table[0x110].len);

    return 0;
}

/*==========================================
 * スキル攻撃エフェクト＆ダメージ
 *------------------------------------------
 */
int clif_skill_damage(dumb_ptr<block_list> src, dumb_ptr<block_list> dst,
        tick_t tick, interval_t sdelay, interval_t ddelay, int damage,
        int div, SkillID skill_id, int skill_lv, int type)
{
    unsigned char buf[64];
    eptr<struct status_change, StatusChange, StatusChange::MAX_STATUSCHANGE> sc_data;

    nullpo_ret(src);
    nullpo_ret(dst);

    sc_data = battle_get_sc_data(dst);

    WBUFW(buf, 0) = 0x1de;
    WBUFW(buf, 2) = static_cast<uint16_t>(skill_id);
    WBUFL(buf, 4) = unwrap<BlockId>(src->bl_id);
    WBUFL(buf, 8) = unwrap<BlockId>(dst->bl_id);
    WBUFL(buf, 12) = static_cast<uint32_t>(tick.time_since_epoch().count());
    WBUFL(buf, 16) = static_cast<uint32_t>(sdelay.count());
    WBUFL(buf, 20) = static_cast<uint32_t>(ddelay.count());
    WBUFL(buf, 24) = damage;
    WBUFW(buf, 28) = skill_lv;
    WBUFW(buf, 30) = div;
    WBUFB(buf, 32) = (type > 0) ? type : skill_get_hit(skill_id);
    clif_send(buf, clif_parse_func_table[0x1de].len, src, SendWho::AREA);

    return 0;
}

/*==========================================
 * 状態異常アイコン/メッセージ表示
 *------------------------------------------
 */
int clif_status_change(dumb_ptr<block_list> bl, StatusChange type, int flag)
{
    unsigned char buf[16];

    nullpo_ret(bl);

    WBUFW(buf, 0) = 0x0196;
    WBUFW(buf, 2) = static_cast<uint16_t>(type);
    WBUFL(buf, 4) = unwrap<BlockId>(bl->bl_id);
    WBUFB(buf, 8) = flag;
    clif_send(buf, clif_parse_func_table[0x196].len, bl, SendWho::AREA);
    return 0;
}

/*==========================================
 * Send message (modified by [Yor])
 *------------------------------------------
 */
void clif_displaymessage(Session *s, XString mes)
{
    if (mes)
    {
        // don't send a void message (it's not displaying on the client chat). @help can send void line.
        WFIFOW(s, 0) = 0x8e;
        size_t str_len = mes.size() + 1; // NUL (might not be NUL yet)
        WFIFOW(s, 2) = 4 + str_len;
        WFIFO_STRING(s, 4, mes, str_len);
        WFIFOSET(s, 4 + str_len);
    }
}

/*==========================================
 * 天の声を送信する
 *------------------------------------------
 */
void clif_GMmessage(dumb_ptr<block_list> bl, XString mes, int flag)
{
    size_t str_len = mes.size() + 1;
    unsigned char buf[str_len + 4];

    WBUFW(buf, 0) = 0x9a;
    WBUFW(buf, 2) = str_len + 4;
    WBUF_STRING(buf, 4, mes, str_len);
    flag &= 0x07;
    clif_send(buf, WBUFW(buf, 2), bl,
               (flag == 1) ? SendWho::ALL_SAMEMAP :
               (flag == 2) ? SendWho::AREA :
               (flag == 3) ? SendWho::SELF :
               SendWho::ALL_CLIENT);
}

/*==========================================
 * 復活する
 *------------------------------------------
 */
void clif_resurrection(dumb_ptr<block_list> bl, int type)
{
    uint8_t buf[16];

    nullpo_retv(bl);

    WBUFW(buf, 0) = 0x148;
    WBUFL(buf, 2) = unwrap<BlockId>(bl->bl_id);
    WBUFW(buf, 6) = type;

    clif_send(buf, clif_parse_func_table[0x148].len, bl,
            type == 1 ? SendWho::AREA : SendWho::AREA_WOS);
}

/*==========================================
 * Wisp/page is transmitted to the destination player
 *------------------------------------------
 */
void clif_wis_message(Session *s, CharName nick, XString mes)   // R 0097 <len>.w <nick>.24B <message>.?B
{
    size_t mes_len = mes.size() + 1;
    WFIFOW(s, 0) = 0x97;
    WFIFOW(s, 2) = mes_len + 24 + 4;
    WFIFO_STRING(s, 4, nick.to__actual(), 24);
    WFIFO_STRING(s, 28, mes, mes_len);
    WFIFOSET(s, WFIFOW(s, 2));
}

/*==========================================
 * The transmission result of Wisp/page is transmitted to the source player
 *------------------------------------------
 */
void clif_wis_end(Session *s, int flag) // R 0098 <type>.B: 0: success to send wisper, 1: target character is not loged in?, 2: ignored by target
{
    WFIFOW(s, 0) = 0x98;
    WFIFOW(s, 2) = flag;
    WFIFOSET(s, clif_parse_func_table[0x98].len);
}

/*==========================================
 * パーティ作成完了
 * Relay the result of party creation.
 *
 * (R 00fa <flag>.B)
 *
 * flag:
 *  0 The party was created.
 *  1 The party name is invalid/taken.
 *  2 The character is already in a party.
 *------------------------------------------
 */
int clif_party_created(dumb_ptr<map_session_data> sd, int flag)
{
    nullpo_ret(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xfa;
    WFIFOB(s, 2) = flag;
    WFIFOSET(s, clif_parse_func_table[0xfa].len);
    return 0;
}

/*==========================================
 * パーティ情報送信
 *------------------------------------------
 */
int clif_party_info(struct party *p, Session *s)
{
    unsigned char buf[1024];
    int i, c;
    dumb_ptr<map_session_data> sd = NULL;

    nullpo_ret(p);

    WBUFW(buf, 0) = 0xfb;
    WBUF_STRING(buf, 4, p->name, 24);
    for (i = c = 0; i < MAX_PARTY; i++)
    {
        struct party_member *m = &p->member[i];
        if (m->account_id)
        {
            if (sd == NULL)
                sd = dumb_ptr<map_session_data>(m->sd);
            WBUFL(buf, 28 + c * 46) = unwrap<AccountId>(m->account_id);
            WBUF_STRING(buf, 28 + c * 46 + 4, m->name.to__actual(), 24);
            WBUF_STRING(buf, 28 + c * 46 + 28, m->map, 16);
            WBUFB(buf, 28 + c * 46 + 44) = (m->leader) ? 0 : 1;
            WBUFB(buf, 28 + c * 46 + 45) = (m->online) ? 0 : 1;
            c++;
        }
    }
    size_t len = 28 + c * 46;
    WBUFW(buf, 2) = len;
    if (s)
    {
        // If set, send only to fd.
        WFIFO_BUF_CLONE(s, buf, len);
        WFIFOSET(s, len);
        return 9;
    }
    // else, send it to all the party, if they exist.
    if (sd != NULL)
        clif_send(buf, len, sd, SendWho::PARTY);
    return 0;
}

/*==========================================
 * パーティ勧誘
 * Relay a party invitation.
 *
 * (R 00fe <sender_ID>.l <party_name>.24B)
 *------------------------------------------
 */
void clif_party_invite(dumb_ptr<map_session_data> sd,
                       dumb_ptr<map_session_data> tsd)
{
    struct party *p;

    nullpo_retv(sd);
    nullpo_retv(tsd);

    Session *s = tsd->sess;

    if (!(p = party_search(sd->status.party_id)))
        return;

    WFIFOW(s, 0) = 0xfe;
    WFIFOL(s, 2) = unwrap<AccountId>(sd->status_key.account_id);
    WFIFO_STRING(s, 6, p->name, 24);
    WFIFOSET(s, clif_parse_func_table[0xfe].len);
}

/*==========================================
 * パーティ勧誘結果
 * Relay the response to a party invitation.
 *
 * (R 00fd <name>.24B <flag>.B)
 *
 * flag:
 *  0 The character is already in a party.
 *  1 The invitation was rejected.
 *  2 The invitation was accepted.
 *  3 The party is full.
 *  4 The character is in the same party.
 *------------------------------------------
 */
void clif_party_inviteack(dumb_ptr<map_session_data> sd, CharName nick, int flag)
{
    nullpo_retv(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xfd;
    WFIFO_STRING(s, 2, nick.to__actual(), 24);
    WFIFOB(s, 26) = flag;
    WFIFOSET(s, clif_parse_func_table[0xfd].len);
}

/*==========================================
 * パーティ設定送信
 * flag & 0x001=exp変更ミス
 *        0x010=item変更ミス
 *        0x100=一人にのみ送信
 *------------------------------------------
 */
void clif_party_option(struct party *p, dumb_ptr<map_session_data> sd, int flag)
{
    unsigned char buf[16];

    nullpo_retv(p);

    if (sd == NULL && flag == 0)
    {
        int i;
        for (i = 0; i < MAX_PARTY; i++)
            if ((sd = map_id2sd(account_to_block(p->member[i].account_id))) != NULL)
                break;
    }
    if (sd == NULL)
        return;
    WBUFW(buf, 0) = 0x101;
    WBUFW(buf, 2) = ((flag & 0x01) ? 2 : p->exp);
    WBUFW(buf, 4) = ((flag & 0x10) ? 2 : p->item);
    if (flag == 0)
        clif_send(buf, clif_parse_func_table[0x101].len, sd, SendWho::PARTY);
    else
    {
        WFIFO_BUF_CLONE(sd->sess, buf, clif_parse_func_table[0x101].len);
        WFIFOSET(sd->sess, clif_parse_func_table[0x101].len);
    }
}

/*==========================================
 * パーティ脱退（脱退前に呼ぶこと）
 *------------------------------------------
 */
void clif_party_leaved(struct party *p, dumb_ptr<map_session_data> sd,
        AccountId account_id, CharName name, int flag)
{
    unsigned char buf[64];
    int i;

    nullpo_retv(p);

    WBUFW(buf, 0) = 0x105;
    WBUFL(buf, 2) = unwrap<AccountId>(account_id);
    WBUF_STRING(buf, 6, name.to__actual(), 24);
    WBUFB(buf, 30) = flag & 0x0f;

    if ((flag & 0xf0) == 0)
    {
        if (sd == NULL)
            for (i = 0; i < MAX_PARTY; i++)
            {
                sd = dumb_ptr<map_session_data>(p->member[i].sd);
                if (sd != NULL)
                    break;
            }
        if (sd != NULL)
            clif_send(buf, clif_parse_func_table[0x105].len, sd, SendWho::PARTY);
    }
    else if (sd != NULL)
    {
        WFIFO_BUF_CLONE(sd->sess, buf, clif_parse_func_table[0x105].len);
        WFIFOSET(sd->sess, clif_parse_func_table[0x105].len);
    }
}

/*==========================================
 * パーティメッセージ送信
 *------------------------------------------
 */
void clif_party_message(struct party *p, AccountId account_id, XString mes)
{
    // always set, but clang is not smart enough
    dumb_ptr<map_session_data> sd = nullptr;
    int i;

    nullpo_retv(p);

    for (i = 0; i < MAX_PARTY; i++)
    {
        sd = dumb_ptr<map_session_data>(p->member[i].sd);
        if (sd != NULL)
            break;
    }
    if (sd != NULL)
    {
        size_t len = mes.size() + 1;
        unsigned char buf[len + 8];
        WBUFW(buf, 0) = 0x109;
        WBUFW(buf, 2) = len + 8;
        WBUFL(buf, 4) = unwrap<AccountId>(account_id);
        WBUF_STRING(buf, 8, mes, len);
        clif_send(buf, len + 8, sd, SendWho::PARTY);
    }
}

/*==========================================
 * パーティ座標通知
 *------------------------------------------
 */
int clif_party_xy(struct party *, dumb_ptr<map_session_data> sd)
{
    unsigned char buf[16];

    nullpo_ret(sd);

    WBUFW(buf, 0) = 0x107;
    WBUFL(buf, 2) = unwrap<AccountId>(sd->status_key.account_id);
    WBUFW(buf, 6) = sd->bl_x;
    WBUFW(buf, 8) = sd->bl_y;
    clif_send(buf, clif_parse_func_table[0x107].len, sd, SendWho::PARTY_SAMEMAP_WOS);
    return 0;
}

/*==========================================
 * パーティHP通知
 *------------------------------------------
 */
int clif_party_hp(struct party *, dumb_ptr<map_session_data> sd)
{
    unsigned char buf[16];

    nullpo_ret(sd);

    WBUFW(buf, 0) = 0x106;
    WBUFL(buf, 2) = unwrap<AccountId>(sd->status_key.account_id);
    WBUFW(buf, 6) = (sd->status.hp > 0x7fff) ? 0x7fff : sd->status.hp;
    WBUFW(buf, 8) =
        (sd->status.max_hp > 0x7fff) ? 0x7fff : sd->status.max_hp;
    clif_send(buf, clif_parse_func_table[0x106].len, sd, SendWho::PARTY_AREA_WOS);
    return 0;
}

/*==========================================
 * 攻撃するために移動が必要
 *------------------------------------------
 */
int clif_movetoattack(dumb_ptr<map_session_data> sd, dumb_ptr<block_list> bl)
{
    nullpo_ret(sd);
    nullpo_ret(bl);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0x139;
    WFIFOL(s, 2) = unwrap<BlockId>(bl->bl_id);
    WFIFOW(s, 6) = bl->bl_x;
    WFIFOW(s, 8) = bl->bl_y;
    WFIFOW(s, 10) = sd->bl_x;
    WFIFOW(s, 12) = sd->bl_y;
    WFIFOW(s, 14) = sd->attackrange;
    WFIFOSET(s, clif_parse_func_table[0x139].len);
    return 0;
}

/*==========================================
 * MVPエフェクト
 *------------------------------------------
 */
int clif_mvp_effect(dumb_ptr<map_session_data> sd)
{
    unsigned char buf[16];

    nullpo_ret(sd);

    WBUFW(buf, 0) = 0x10c;
    WBUFL(buf, 2) = unwrap<BlockId>(sd->bl_id);
    clif_send(buf, clif_parse_func_table[0x10c].len, sd, SendWho::AREA);
    return 0;
}

/*==========================================
 * エモーション
 *------------------------------------------
 */
void clif_emotion(dumb_ptr<block_list> bl, int type)
{
    unsigned char buf[8];

    nullpo_retv(bl);

    WBUFW(buf, 0) = 0xc0;
    WBUFL(buf, 2) = unwrap<BlockId>(bl->bl_id);
    WBUFB(buf, 6) = type;
    clif_send(buf, clif_parse_func_table[0xc0].len, bl, SendWho::AREA);
}

static
void clif_emotion_towards(dumb_ptr<block_list> bl,
                                  dumb_ptr<block_list> target, int type)
{
    unsigned char buf[8];
    int len = clif_parse_func_table[0xc0].len;
    dumb_ptr<map_session_data> sd = target->is_player();

    nullpo_retv(bl);
    nullpo_retv(target);

    if (target->bl_type != BL::PC)
        return;

    WBUFW(buf, 0) = 0xc0;
    WBUFL(buf, 2) = unwrap<BlockId>(bl->bl_id);
    WBUFB(buf, 6) = type;

    WFIFO_BUF_CLONE(sd->sess, buf, len);
    WFIFOSET(sd->sess, len);
}

/*==========================================
 * 座る
 *------------------------------------------
 */
void clif_sitting(Session *, dumb_ptr<map_session_data> sd)
{
    unsigned char buf[64];

    nullpo_retv(sd);

    WBUFW(buf, 0) = 0x8a;
    WBUFL(buf, 2) = unwrap<BlockId>(sd->bl_id);
    WBUFB(buf, 26) = 2;
    clif_send(buf, clif_parse_func_table[0x8a].len, sd, SendWho::AREA);
}

/*==========================================
 *
 *------------------------------------------
 */
static
int clif_GM_kickack(dumb_ptr<map_session_data> sd, AccountId id)
{
    nullpo_ret(sd);

    Session *s = sd->sess;
    WFIFOW(s, 0) = 0xcd;
    WFIFOL(s, 2) = unwrap<AccountId>(id);
    WFIFOSET(s, clif_parse_func_table[0xcd].len);
    return 0;
}

static
void clif_parse_QuitGame(Session *s, dumb_ptr<map_session_data> sd);

int clif_GM_kick(dumb_ptr<map_session_data> sd, dumb_ptr<map_session_data> tsd,
                  int type)
{
    nullpo_ret(tsd);

    if (type)
        clif_GM_kickack(sd, tsd->status_key.account_id);
    tsd->opt1 = Opt1::ZERO;
    tsd->opt2 = Opt2::ZERO;
    clif_parse_QuitGame(tsd->sess, tsd);

    return 0;
}

// displaying special effects (npcs, weather, etc) [Valaris]
int clif_specialeffect(dumb_ptr<block_list> bl, int type, int flag)
{
    unsigned char buf[24];

    nullpo_ret(bl);

    WBUF_ZERO(buf, 0, clif_parse_func_table[0x19b].len);

    WBUFW(buf, 0) = 0x19b;
    WBUFL(buf, 2) = unwrap<BlockId>(bl->bl_id);
    WBUFL(buf, 6) = type;

    if (flag == 2)
    {
        for (io::FD i : iter_fds())
        {
            Session *s = get_session(i);
            if (!s)
                continue;
            dumb_ptr<map_session_data> sd = dumb_ptr<map_session_data>(static_cast<map_session_data *>(s->session_data.get()));
            if (sd && sd->state.auth && sd->bl_m == bl->bl_m)
                clif_specialeffect(sd, type, 1);
        }
    }
    else if (flag == 1)
        clif_send(buf, clif_parse_func_table[0x19b].len, bl, SendWho::SELF);
    else if (!flag)
        clif_send(buf, clif_parse_func_table[0x19b].len, bl, SendWho::AREA);

    return 0;

}

// ------------
// clif_parse_*
// ------------
// パケット読み取って色々操作
/*==========================================
 *
 *------------------------------------------
 */
static
void clif_parse_WantToConnection(Session *s, dumb_ptr<map_session_data> sd)
{
    AccountId account_id;            // account_id in the packet

    if (sd)
    {
        if (battle_config.error_log)
            PRINTF("clif_parse_WantToConnection : invalid request?\n"_fmt);
        return;
    }

    if (RFIFOW(s, 0) == 0x72)
    {
        account_id = wrap<AccountId>(RFIFOL(s, 2));
    }
    else
        return;                 // Not the auth packet

    WFIFOL(s, 0) = unwrap<AccountId>(account_id);
    WFIFOSET(s, 4);

    // if same account already connected, we disconnect the 2 sessions
    dumb_ptr<map_session_data> old_sd = map_id2sd(account_to_block(account_id));
    if (old_sd)
    {
        clif_authfail_fd(s, 2);   // same id
        clif_authfail_fd(old_sd->sess, 2);   // same id
        PRINTF("clif_parse_WantToConnection: Double connection for account %d (sessions: #%d (new) and #%d (old)).\n"_fmt,
             account_id, s, old_sd->sess);
    }
    else
    {
        sd.new_();
        s->session_data.reset(sd.operator->());
        sd->sess = s;

        pc_setnewpc(sd, account_id, wrap<CharId>(RFIFOL(s, 6)), RFIFOL(s, 10),
                tick_t(static_cast<interval_t>(RFIFOL(s, 14))),
                static_cast<SEX>(RFIFOB(s, 18)));

        map_addiddb(sd);

        chrif_authreq(sd);
    }

    return;
}

/*==========================================
 * 007d クライアント側マップ読み込み完了
 * map侵入時に必要なデータを全て送りつける
 *------------------------------------------
 */
static
void clif_parse_LoadEndAck(Session *, dumb_ptr<map_session_data> sd)
{
    nullpo_retv(sd);

    if (sd->bl_prev != NULL)
        return;

    // 接続ok時
    //clif_authok();
    if (sd->npc_id)
        npc_event_dequeue(sd);
    clif_skillinfoblock(sd);
    pc_checkitem(sd);
    //guild_info();

    // loadendack時
    // next exp
    clif_updatestatus(sd, SP::NEXTBASEEXP);
    clif_updatestatus(sd, SP::NEXTJOBEXP);
    // skill point
    clif_updatestatus(sd, SP::SKILLPOINT);
    // item
    clif_itemlist(sd);
    clif_equiplist(sd);
    // param all
    clif_initialstatus(sd);
    // party
    party_send_movemap(sd);
    // 119
    // 78

    if (battle_config.player_invincible_time > 0)
    {
        pc_setinvincibletimer(sd, static_cast<interval_t>(battle_config.player_invincible_time));
    }

    map_addblock(sd);     // ブロック登録
    clif_spawnpc(sd);          // spawn

    // weight max , now
    clif_updatestatus(sd, SP::MAXWEIGHT);
    clif_updatestatus(sd, SP::WEIGHT);

    // pvp
    if (!battle_config.pk_mode)
        sd->pvp_timer.cancel();

    if (sd->bl_m->flag.get(MapFlag::PVP))
    {
        if (!battle_config.pk_mode)
        {
            // remove pvp stuff for pk_mode [Valaris]
            sd->pvp_timer = Timer(gettick() + std::chrono::milliseconds(200),
                    std::bind(pc_calc_pvprank_timer, ph::_1, ph::_2,
                        sd->bl_id));
            sd->pvp_rank = 0;
            sd->pvp_lastusers = 0;
            sd->pvp_point = 5;
        }
    }
    else
    {
        // sd->pvp_timer = nullptr;
    }

    sd->state.connect_new = 0;

    // view equipment item
    clif_changelook(sd, LOOK::WEAPON, static_cast<uint16_t>(ItemLook::NONE));
    if (battle_config.save_clothcolor == 1 && sd->status.clothes_color > 0)
        clif_changelook(sd, LOOK::CLOTHES_COLOR,
                         sd->status.clothes_color);

    // option
    clif_changeoption(sd);
    // broken equipment

//        clif_changelook_accessories(sd, NULL);

    map_foreachinarea(std::bind(clif_getareachar, ph::_1, sd),
            sd->bl_m,
            sd->bl_x - AREA_SIZE, sd->bl_y - AREA_SIZE,
            sd->bl_x + AREA_SIZE, sd->bl_y + AREA_SIZE,
            BL::NUL);

    if (!sd->state.seen_motd)
        pc_show_motd(sd);
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_parse_TickSend(Session *s, dumb_ptr<map_session_data> sd)
{
    nullpo_retv(sd);

    sd->client_tick = tick_t(static_cast<interval_t>(RFIFOL(s, 2)));
    sd->server_tick = gettick();
    clif_servertick(sd);
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_parse_WalkToXY(Session *s, dumb_ptr<map_session_data> sd)
{
    int x, y;

    nullpo_retv(sd);

    if (pc_isdead(sd))
    {
        clif_clearchar(sd, BeingRemoveWhy::DEAD);
        return;
    }

    if (sd->npc_id || sd->state.storage_open)
        return;

    if (sd->canmove_tick > gettick())
        return;

    // ステータス異常やハイディング中(トンネルドライブ無)で動けない
    if (bool(sd->opt1) && sd->opt1 != (Opt1::_stone6))
        return;

    if (sd->invincible_timer)
        pc_delinvincibletimer(sd);

    pc_stopattack(sd);

    x = RFIFOB(s, 2) * 4 + (RFIFOB(s, 3) >> 6);
    y = ((RFIFOB(s, 3) & 0x3f) << 4) + (RFIFOB(s, 4) >> 4);
    pc_walktoxy(sd, x, y);

}

/*==========================================
 *
 *------------------------------------------
 */
void clif_parse_QuitGame(Session *s, dumb_ptr<map_session_data> sd)
{
    tick_t tick = gettick();

    nullpo_retv(sd);

    WFIFOW(s, 0) = 0x18b;
    if ((!pc_isdead(sd) && (sd->opt1 != Opt1::ZERO || sd->opt2 != Opt2::ZERO))
        || (tick < sd->canact_tick))
    {
        WFIFOW(s, 2) = 1;
        WFIFOSET(s, clif_parse_func_table[0x18b].len);
        return;
    }

    /*  Rovert's prevent logout option fixed [Valaris]  */
    if (!battle_config.prevent_logout
        || tick >= sd->canlog_tick + std::chrono::seconds(10))
    {
        clif_setwaitclose(s);
        WFIFOW(s, 2) = 0;
    }
    else
    {
        WFIFOW(s, 2) = 1;
    }
    WFIFOSET(s, clif_parse_func_table[0x18b].len);

}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_parse_GetCharNameRequest(Session *s, dumb_ptr<map_session_data> sd)
{
    dumb_ptr<block_list> bl;
    BlockId account_id;

    account_id = wrap<BlockId>(RFIFOL(s, 2));
    bl = map_id2bl(account_id);
    if (bl == NULL)
        return;

    WFIFOW(s, 0) = 0x95;
    WFIFOL(s, 2) = unwrap<BlockId>(account_id);

    switch (bl->bl_type)
    {
        case BL::PC:
        {
            dumb_ptr<map_session_data> ssd = bl->is_player();

            nullpo_retv(ssd);

            if (ssd->state.shroud_active)
                WFIFO_STRING(s, 6, ""_s, 24);
            else
                WFIFO_STRING(s, 6, ssd->status_key.name.to__actual(), 24);
            WFIFOSET(s, clif_parse_func_table[0x95].len);

            struct party *p = NULL;

            PartyName party_name;

            int send = 0;

            if (ssd->status.party_id && (p = party_search(ssd->status.party_id)) != NULL)
            {
                party_name = p->name;
                send = 1;
            }

            if (send)
            {
                WFIFOW(s, 0) = 0x195;
                WFIFOL(s, 2) = unwrap<BlockId>(account_id);
                WFIFO_STRING(s, 6, party_name, 24);
                WFIFO_STRING(s, 30, ""_s, 24);
                WFIFO_STRING(s, 54, ""_s, 24);
                WFIFO_STRING(s, 78, ""_s, 24); // We send this value twice because the client expects it
                WFIFOSET(s, clif_parse_func_table[0x195].len);

            }

            if (pc_isGM(sd).satisfies(GmLevel::from(static_cast<uint32_t>(battle_config.hack_info_GM_level))))
            {
                IP4Address ip = ssd->get_ip();
                WFIFOW(s, 0) = 0x20C;

                // Mask the IP using the char-server password
                if (battle_config.mask_ip_gms)
                    ip = MD5_ip(ip);

                WFIFOL(s, 2) = unwrap<BlockId>(account_id);
                WFIFOIP(s, 6) = ip;
                WFIFOSET(s, clif_parse_func_table[0x20C].len);
             }

        }
            break;
        case BL::NPC:
        {
            NpcName name = bl->is_npc()->name;
            // [fate] elim hashed out/invisible names for the client
            auto it = std::find(name.begin(), name.end(), '#');
            WFIFO_STRING(s, 6, name.xislice_h(it), 24);
            WFIFOSET(s, clif_parse_func_table[0x95].len);
        }
            break;
        case BL::MOB:
        {
            dumb_ptr<mob_data> md = bl->is_mob();

            nullpo_retv(md);

            WFIFO_STRING(s, 6, md->name, 24);
            WFIFOSET(s, clif_parse_func_table[0x95].len);
        }
            break;
        default:
            if (battle_config.error_log)
                PRINTF("clif_parse_GetCharNameRequest : bad type %d (%d)\n"_fmt,
                        bl->bl_type, account_id);
            break;
    }
}

/*==========================================
 * Validate and process transmission of a
 * global/public message.
 *
 * (S 008c <len>.w <message>.?B)
 *------------------------------------------
 */
static
void clif_parse_GlobalMessage(Session *s, dumb_ptr<map_session_data> sd)
{
    nullpo_retv(sd);

    AString mbuf = clif_validate_chat(sd, ChatType::Global);
    if (!mbuf)
    {
        clif_displaymessage(s, "Your message could not be sent."_s);
        return;
    }

    if (is_atcommand(s, sd, mbuf, GmLevel()))
        return;

    if (!magic_message(sd, mbuf))
    {
        /* Don't send chat that results in an automatic ban. */
        if (tmw_CheckChatSpam(sd, mbuf))
        {
            clif_displaymessage(s, "Your message could not be sent."_s);
            return;
        }

        /* It's not a spell/magic message, so send the message to others. */
        size_t mbuf_size = mbuf.size() + 1;
        uint8_t sendbuf[mbuf_size + 8];
        WBUFW(sendbuf, 0) = 0x8d;
        WBUFW(sendbuf, 2) = mbuf_size + 8;   /* Header(2) + length(2) + ID(4). */
        WBUFL(sendbuf, 4) = unwrap<BlockId>(sd->bl_id);
        WBUF_STRING(sendbuf, 8, mbuf, mbuf_size);

        clif_send(sendbuf, mbuf_size + 8, sd, SendWho::AREA_CHAT_WOC);
    }

    /* Send the message back to the speaker. */
    size_t len = RFIFOW(s, 2);
    RFIFO_WFIFO_CLONE(s, s, len);
    WFIFOW(s, 0) = 0x8e;
    WFIFOSET(s, len);
}

void clif_message(dumb_ptr<block_list> bl, XString msg)
{
    size_t msg_len = msg.size() + 1;
    uint8_t buf[512];

    if (msg_len + 16 > 512)
        return;

    nullpo_retv(bl);

    WBUFW(buf, 0) = 0x8d;
    WBUFW(buf, 2) = msg_len + 8;
    WBUFL(buf, 4) = unwrap<BlockId>(bl->bl_id);
    WBUF_STRING(buf, 8, msg, msg_len);

    clif_send(buf, WBUFW(buf, 2), bl, SendWho::AREA);
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_parse_ChangeDir(Session *s, dumb_ptr<map_session_data> sd)
{
    unsigned char buf[64];

    nullpo_retv(sd);

    // RFIFOW(fd, 2) and WBUFW(buf, 6) are always 0
    // TODO perhaps we could use that to remove this hack?
    DIR dir;
    uint8_t client_dir = RFIFOB(s, 4);
    // the client uses a diffenent direction enum ... ugh
    switch (client_dir)
    {
    case 1 | 0: dir = DIR::S; break; // down
    case 1 | 2: dir = DIR::SW; break;
    case 0 | 2: dir = DIR::W; break; // left
    case 4 | 2: dir = DIR::NW; break;
    case 4 | 0: dir = DIR::N; break; // up
    case 4 | 8: dir = DIR::NE; break;
    case 0 | 8: dir = DIR::E; break; // right
    case 1 | 8: dir = DIR::SE; break;
    default:
        return;
    }

    if (dir == sd->dir)
        return;

    pc_setdir(sd, dir);

    WBUFW(buf, 0) = 0x9c;
    WBUFL(buf, 2) = unwrap<BlockId>(sd->bl_id);
    WBUFW(buf, 6) = 0;
    WBUFB(buf, 8) = client_dir;

    clif_send(buf, clif_parse_func_table[0x9c].len, sd, SendWho::AREA_WOS);

}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_parse_Emotion(Session *s, dumb_ptr<map_session_data> sd)
{
    unsigned char buf[64];

    nullpo_retv(sd);

    if (battle_config.basic_skill_check == 0
        || pc_checkskill(sd, SkillID::NV_EMOTE) >= 1)
    {
        uint8_t emote = RFIFOB(s, 2);
        WBUFW(buf, 0) = 0xc0;
        WBUFL(buf, 2) = unwrap<BlockId>(sd->bl_id);
        WBUFB(buf, 6) = emote;
        clif_send(buf, clif_parse_func_table[0xc0].len, sd, SendWho::AREA);
    }
    else
        clif_skill_fail(sd, SkillID::ONE, 0, 1);
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_parse_HowManyConnections(Session *s, dumb_ptr<map_session_data>)
{
    WFIFOW(s, 0) = 0xc2;
    WFIFOL(s, 2) = map_getusers();
    WFIFOSET(s, clif_parse_func_table[0xc2].len);
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_parse_ActionRequest(Session *s, dumb_ptr<map_session_data> sd)
{
    unsigned char buf[64];
    int action_type;
    BlockId target_id;

    nullpo_retv(sd);

    if (pc_isdead(sd))
    {
        clif_clearchar(sd, BeingRemoveWhy::DEAD);
        return;
    }
    if (sd->npc_id
        || bool(sd->opt1)
        || sd->state.storage_open)
        return;

    tick_t tick = gettick();

    pc_stop_walking(sd, 0);
    pc_stopattack(sd);

    target_id = wrap<BlockId>(RFIFOL(s, 2));
    action_type = RFIFOB(s, 6);

    switch (action_type)
    {
        case 0x00:             // once attack
        case 0x07:             // continuous attack
            if (bool(sd->status.option & Option::HIDE))
                return;
            if (!battle_config.skill_delay_attack_enable)
            {
                if (tick < sd->canact_tick)
                {
                    clif_skill_fail(sd, SkillID::ONE, 4, 0);
                    return;
                }
            }
            if (sd->invincible_timer)
                pc_delinvincibletimer(sd);
            sd->attacktarget = BlockId();
            pc_attack(sd, target_id, action_type != 0);
            break;
        case 0x02:             // sitdown
            pc_stop_walking(sd, 1);
            pc_setsit(sd);
            clif_sitting(s, sd);
            break;
        case 0x03:             // standup
            pc_setstand(sd);
            WBUFW(buf, 0) = 0x8a;
            WBUFL(buf, 2) = unwrap<BlockId>(sd->bl_id);
            WBUFB(buf, 26) = 3;
            clif_send(buf, clif_parse_func_table[0x8a].len, sd, SendWho::AREA);
            break;
    }
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_parse_Restart(Session *s, dumb_ptr<map_session_data> sd)
{
    nullpo_retv(sd);

    switch (RFIFOB(s, 2))
    {
        case 0x00:
            if (pc_isdead(sd))
            {
                pc_setstand(sd);
                pc_setrestartvalue(sd, 3);
                if (sd->bl_m->flag.get(MapFlag::RESAVE))
                {
                    pc_setpos(sd, sd->bl_m->resave.map_,
                            sd->bl_m->resave.x, sd->bl_m->resave.y,
                            BeingRemoveWhy::QUIT);
                }
                else
                {
                    pc_setpos(sd, sd->status.save_point.map_,
                            sd->status.save_point.x, sd->status.save_point.y,
                            BeingRemoveWhy::QUIT);
                }
            }
            break;
        case 0x01:
            /*  Rovert's Prevent logout option - Fixed [Valaris]    */
            if (!battle_config.prevent_logout
                || gettick() >= sd->canlog_tick + std::chrono::seconds(10))
            {
                chrif_charselectreq(sd);
            }
            else
            {
                WFIFOW(s, 0) = 0x18b;
                WFIFOW(s, 2) = 1;

                WFIFOSET(s, clif_parse_func_table[0x018b].len);
            }
            break;
    }
}

/*==========================================
 * Validate and process transmission of a
 * whisper/private message.
 *
 * (S 0096 <len>.w <nick>.24B <message>.?B)
 *
 * rewritten by [Yor], then partially by
 * [remoitnane]
 *------------------------------------------
 */
static
void clif_parse_Wis(Session *s, dumb_ptr<map_session_data> sd)
{
    dumb_ptr<map_session_data> dstsd = NULL;

    nullpo_retv(sd);

    AString mbuf = clif_validate_chat(sd, ChatType::Whisper);
    if (!mbuf)
    {
        clif_displaymessage(s, "Your message could not be sent."_s);
        return;
    }

    if (is_atcommand(s, sd, mbuf, GmLevel()))
    {
        return;
    }

    /* Don't send chat that results in an automatic ban. */
    if (tmw_CheckChatSpam(sd, mbuf))
    {
        clif_displaymessage(s, "Your message could not be sent."_s);
        return;
    }

    /*
     * The player is not on this server. Only send the whisper if the name is
     * exactly the same, because if there are multiple map-servers and a name
     * conflict (for instance, "Test" versus "test"), the char-server must
     * settle the discrepancy.
     */
    CharName tname = stringish<CharName>(RFIFO_STRING<24>(s, 4));
    if (!(dstsd = map_nick2sd(tname))
            || dstsd->status_key.name != tname)
        intif_wis_message(sd, tname, mbuf);
    else
    {
        /* Refuse messages addressed to self. */
        if (dstsd->sess == s)
        {
            ZString mes = "You cannot page yourself."_s;
            clif_wis_message(s, wisp_server_name, mes);
        }
        else
        {
            {
                /* The player is not being ignored. */
                {
                    clif_wis_message(dstsd->sess, sd->status_key.name, mbuf);
                    /* The whisper was sent successfully. */
                    clif_wis_end(s, 0);
                }
            }
        }
    }
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_parse_TakeItem(Session *s, dumb_ptr<map_session_data> sd)
{
    dumb_ptr<flooritem_data> fitem;

    nullpo_retv(sd);

    BlockId map_object_id = wrap<BlockId>(RFIFOL(s, 2));
    fitem = map_id_is_item(map_object_id);

    if (pc_isdead(sd))
    {
        clif_clearchar(sd, BeingRemoveWhy::DEAD);
        return;
    }

    if (sd->npc_id
        || sd->opt1 != Opt1::ZERO)   //会話禁止
        return;

    if (fitem == NULL || fitem->bl_m != sd->bl_m)
        return;

    if (abs(sd->bl_x - fitem->bl_x) >= 2
        || abs(sd->bl_y - fitem->bl_y) >= 2)
        return;                 // too far away to pick up

    if (sd->state.shroud_active && sd->state.shroud_disappears_on_pickup)
        magic_unshroud(sd);

    pc_takeitem(sd, fitem);
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_parse_DropItem(Session *s, dumb_ptr<map_session_data> sd)
{
    int item_index, item_amount;

    nullpo_retv(sd);

    if (pc_isdead(sd))
    {
        clif_clearchar(sd, BeingRemoveWhy::DEAD);
        return;
    }
    if (sd->bl_m->flag.get(MapFlag::NO_PLAYER_DROPS))
    {
        clif_displaymessage(sd->sess, "Can't drop items here."_s);
        return;
    }
    if (sd->npc_id
        || sd->opt1 != Opt1::ZERO)
    {
        clif_displaymessage(sd->sess, "Can't drop items right now."_s);
        return;
    }

    item_index = RFIFOW(s, 2) - 2;
    item_amount = RFIFOW(s, 4);

    pc_dropitem(sd, item_index, item_amount);
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_parse_UseItem(Session *s, dumb_ptr<map_session_data> sd)
{
    nullpo_retv(sd);

    if (pc_isdead(sd))
    {
        clif_clearchar(sd, BeingRemoveWhy::DEAD);
        return;
    }
    if (sd->npc_id
        || sd->opt1 != Opt1::ZERO)   //会話禁止
        return;

    if (sd->invincible_timer)
        pc_delinvincibletimer(sd);

    pc_useitem(sd, RFIFOW(s, 2) - 2);
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_parse_EquipItem(Session *s, dumb_ptr<map_session_data> sd)
{
    int index;

    nullpo_retv(sd);

    if (pc_isdead(sd))
    {
        clif_clearchar(sd, BeingRemoveWhy::DEAD);
        return;
    }
    index = RFIFOW(s, 2) - 2;
    if (sd->npc_id)
        return;

    if (sd->inventory_data[index])
    {
        EPOS epos = EPOS(RFIFOW(s, 4));
        if (sd->inventory_data[index]->type == ItemType::ARROW)
            epos = EPOS::ARROW;

        // Note: the EPOS argument to pc_equipitem is actually ignored
        pc_equipitem(sd, index, epos);
    }
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_parse_UnequipItem(Session *s, dumb_ptr<map_session_data> sd)
{
    int index;

    nullpo_retv(sd);

    if (pc_isdead(sd))
    {
        clif_clearchar(sd, BeingRemoveWhy::DEAD);
        return;
    }
    index = RFIFOW(s, 2) - 2;

    if (sd->npc_id
        || sd->opt1 != Opt1::ZERO)
        return;
    pc_unequipitem(sd, index, CalcStatus::NOW);
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_parse_NpcClicked(Session *s, dumb_ptr<map_session_data> sd)
{
    nullpo_retv(sd);

    if (pc_isdead(sd))
    {
        clif_clearchar(sd, BeingRemoveWhy::DEAD);
        return;
    }
    if (sd->npc_id)
        return;
    npc_click(sd, wrap<BlockId>(RFIFOL(s, 2)));
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_parse_NpcBuySellSelected(Session *s, dumb_ptr<map_session_data> sd)
{
    npc_buysellsel(sd, wrap<BlockId>(RFIFOL(s, 2)), RFIFOB(s, 6));
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_parse_NpcBuyListSend(Session *s, dumb_ptr<map_session_data> sd)
{
    int n = (RFIFOW(s, 2) - 4) / 4;
    // really an array of pairs of uint16_t
    const uint16_t *item_list = static_cast<const uint16_t *>(RFIFOP(s, 4));

    int fail = npc_buylist(sd, n, item_list);

    WFIFOW(s, 0) = 0xca;
    WFIFOB(s, 2) = fail;
    WFIFOSET(s, clif_parse_func_table[0xca].len);
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_parse_NpcSellListSend(Session *s, dumb_ptr<map_session_data> sd)
{
    int n = (RFIFOW(s, 2) - 4) / 4;
    // really an array of pairs of uint16_t
    const uint16_t *item_list = static_cast<const uint16_t *>(RFIFOP(s, 4));

    int fail = npc_selllist(sd, n, item_list);

    WFIFOW(s, 0) = 0xcb;
    WFIFOB(s, 2) = fail;
    WFIFOSET(s, clif_parse_func_table[0xcb].len);
}

/*==========================================
 * 取引要請を相手に送る
 *------------------------------------------
 */
static
void clif_parse_TradeRequest(Session *, dumb_ptr<map_session_data> sd)
{
    nullpo_retv(sd);

    if (battle_config.basic_skill_check == 0
        || pc_checkskill(sd, SkillID::NV_TRADE) >= 1)
    {
        trade_traderequest(sd, wrap<BlockId>(RFIFOL(sd->sess, 2)));
    }
    else
        clif_skill_fail(sd, SkillID::ONE, 0, 0);
}

/*==========================================
 * 取引要請
 *------------------------------------------
 */
static
void clif_parse_TradeAck(Session *, dumb_ptr<map_session_data> sd)
{
    nullpo_retv(sd);

    trade_tradeack(sd, RFIFOB(sd->sess, 2));
}

/*==========================================
 * アイテム追加
 *------------------------------------------
 */
static
void clif_parse_TradeAddItem(Session *, dumb_ptr<map_session_data> sd)
{
    nullpo_retv(sd);

    trade_tradeadditem(sd, RFIFOW(sd->sess, 2), RFIFOL(sd->sess, 4));
}

/*==========================================
 * アイテム追加完了(ok押し)
 *------------------------------------------
 */
static
void clif_parse_TradeOk(Session *, dumb_ptr<map_session_data> sd)
{
    trade_tradeok(sd);
}

/*==========================================
 * 取引キャンセル
 *------------------------------------------
 */
static
void clif_parse_TradeCansel(Session *, dumb_ptr<map_session_data> sd)
{
    trade_tradecancel(sd);
}

/*==========================================
 * 取引許諾(trade押し)
 *------------------------------------------
 */
static
void clif_parse_TradeCommit(Session *, dumb_ptr<map_session_data> sd)
{
    trade_tradecommit(sd);
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_parse_StopAttack(Session *, dumb_ptr<map_session_data> sd)
{
    pc_stopattack(sd);
}

/*==========================================
 * ステータスアップ
 *------------------------------------------
 */
static
void clif_parse_StatusUp(Session *s, dumb_ptr<map_session_data> sd)
{
    pc_statusup(sd, static_cast<SP>(RFIFOW(s, 2)));
}

/*==========================================
 * スキルレベルアップ
 *------------------------------------------
 */
static
void clif_parse_SkillUp(Session *s, dumb_ptr<map_session_data> sd)
{
    pc_skillup(sd, SkillID(RFIFOW(s, 2)));
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_parse_NpcSelectMenu(Session *s, dumb_ptr<map_session_data> sd)
{
    nullpo_retv(sd);

    sd->npc_menu = RFIFOB(s, 6);
    map_scriptcont(sd, wrap<BlockId>(RFIFOL(s, 2)));
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_parse_NpcNextClicked(Session *s, dumb_ptr<map_session_data> sd)
{
    map_scriptcont(sd, wrap<BlockId>(RFIFOL(s, 2)));
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_parse_NpcAmountInput(Session *s, dumb_ptr<map_session_data> sd)
{
    nullpo_retv(sd);

    sd->npc_amount = RFIFOL(s, 6);
    map_scriptcont(sd, wrap<BlockId>(RFIFOL(s, 2)));
}

/*==========================================
 * Process string-based input for an NPC.
 *
 * (S 01d5 <len>.w <npc_ID>.l <message>.?B)
 *------------------------------------------
 */
static
void clif_parse_NpcStringInput(Session *s, dumb_ptr<map_session_data> sd)
{
    int len;
    nullpo_retv(sd);

    len = RFIFOW(s, 2) - 8;

    /*
     * If we check for equal to 0, too, we'll freeze clients that send (or
     * claim to have sent) an "empty" message.
     */
    if (len < 0)
        return;
    sd->npc_str = RFIFO_STRING(s, 8, len);

    map_scriptcont(sd, wrap<BlockId>(RFIFOL(s, 4)));
}

/*==========================================
 *
 *------------------------------------------
 */
static
void clif_parse_NpcCloseClicked(Session *s, dumb_ptr<map_session_data> sd)
{
    map_scriptcont(sd, wrap<BlockId>(RFIFOL(s, 2)));
}

/*==========================================
 * カプラ倉庫へ入れる
 *------------------------------------------
 */
static
void clif_parse_MoveToKafra(Session *s, dumb_ptr<map_session_data> sd)
{
    int item_index, item_amount;

    nullpo_retv(sd);

    item_index = RFIFOW(s, 2) - 2;
    item_amount = RFIFOL(s, 4);

    if ((sd->npc_id && !sd->npc_flags.storage) || sd->trade_partner
        || !sd->state.storage_open)
        return;

    if (sd->state.storage_open)
        storage_storageadd(sd, item_index, item_amount);
}

/*==========================================
 * カプラ倉庫から出す
 *------------------------------------------
 */
static
void clif_parse_MoveFromKafra(Session *s, dumb_ptr<map_session_data> sd)
{
    int item_index, item_amount;

    nullpo_retv(sd);

    item_index = RFIFOW(s, 2) - 1;
    item_amount = RFIFOL(s, 4);

    if ((sd->npc_id && !sd->npc_flags.storage) || sd->trade_partner
        || !sd->state.storage_open)
        return;

    if (sd->state.storage_open)
        storage_storageget(sd, item_index, item_amount);
}

/*==========================================
 * カプラ倉庫を閉じる
 *------------------------------------------
 */
static
void clif_parse_CloseKafra(Session *, dumb_ptr<map_session_data> sd)
{
    nullpo_retv(sd);

    if (sd->state.storage_open)
        storage_storageclose(sd);
}

/*==========================================
 * パーティを作る
 * Process request to create a party.
 *
 * (S 00f9 <party_name>.24B)
 *------------------------------------------
 */
static
void clif_parse_CreateParty(Session *s, dumb_ptr<map_session_data> sd)
{
    if (battle_config.basic_skill_check == 0
        || pc_checkskill(sd, SkillID::NV_PARTY) >= 2)
    {
        PartyName name = stringish<PartyName>(RFIFO_STRING<24>(s, 2));
        party_create(sd, name);
    }
    else
        clif_skill_fail(sd, SkillID::ONE, 0, 4);
}

/*==========================================
 * パーティに勧誘
 * Process invitation to join a party.
 *
 * (S 00fc <account_ID>.l)
 *------------------------------------------
 */
static
void clif_parse_PartyInvite(Session *s, dumb_ptr<map_session_data> sd)
{
    party_invite(sd, wrap<AccountId>(RFIFOL(s, 2)));
}

/*==========================================
 * パーティ勧誘返答
 * Process reply to party invitation.
 *
 * (S 00ff <account_ID>.l <flag>.l)
 *------------------------------------------
 */
static
void clif_parse_ReplyPartyInvite(Session *s, dumb_ptr<map_session_data> sd)
{
    if (battle_config.basic_skill_check == 0
        || pc_checkskill(sd, SkillID::NV_PARTY) >= 1)
    {
        party_reply_invite(sd, wrap<AccountId>(RFIFOL(s, 2)), RFIFOL(s, 6));
    }
    else
    {
        party_reply_invite(sd, wrap<AccountId>(RFIFOL(s, 2)), 0);
        clif_skill_fail(sd, SkillID::ONE, 0, 4);
    }
}

/*==========================================
 * パーティ脱退要求
 *------------------------------------------
 */
static
void clif_parse_LeaveParty(Session *, dumb_ptr<map_session_data> sd)
{
    party_leave(sd);
}

/*==========================================
 * パーティ除名要求
 *------------------------------------------
 */
static
void clif_parse_RemovePartyMember(Session *s, dumb_ptr<map_session_data> sd)
{
    AccountId account_id = wrap<AccountId>(RFIFOL(s, 2));
    // unused RFIFO_STRING<24>(fd, 6);
    party_removemember(sd, account_id);
}

/*==========================================
 * パーティ設定変更要求
 *------------------------------------------
 */
static
void clif_parse_PartyChangeOption(Session *s, dumb_ptr<map_session_data> sd)
{
    party_changeoption(sd, RFIFOW(s, 2), RFIFOW(s, 4));
}

/*==========================================
 * パーティメッセージ送信要求
 * Validate and process transmission of a
 * party message.
 *
 * (S 0108 <len>.w <message>.?B)
 *------------------------------------------
 */
static
void clif_parse_PartyMessage(Session *s, dumb_ptr<map_session_data> sd)
{
    nullpo_retv(sd);

    AString mbuf = clif_validate_chat(sd, ChatType::Party);
    if (!mbuf)
    {
        clif_displaymessage(s, "Your message could not be sent."_s);
        return;
    }

    if (is_atcommand(s, sd, mbuf, GmLevel()))
        return;

    /* Don't send chat that results in an automatic ban. */
    if (tmw_CheckChatSpam(sd, mbuf))
    {
        clif_displaymessage(s, "Your message could not be sent."_s);
        return;
    }

    party_send_message(sd, mbuf);
}

func_table clif_parse_func_table[0x0220] =
{
    {0,     10, NULL,                           },  // 0x0000
    {0,     0,  NULL,                           },  // 0x0001
    {0,     0,  NULL,                           },  // 0x0002
    {0,     0,  NULL,                           },  // 0x0003
    {0,     0,  NULL,                           },  // 0x0004
    {0,     0,  NULL,                           },  // 0x0005
    {0,     0,  NULL,                           },  // 0x0006
    {0,     0,  NULL,                           },  // 0x0007
    {0,     0,  NULL,                           },  // 0x0008
    {0,     0,  NULL,                           },  // 0x0009
    {0,     0,  NULL,                           },  // 0x000a
    {0,     0,  NULL,                           },  // 0x000b
    {0,     0,  NULL,                           },  // 0x000c
    {0,     0,  NULL,                           },  // 0x000d
    {0,     0,  NULL,                           },  // 0x000e
    {0,     0,  NULL,                           },  // 0x000f
    {0,     0,  NULL,                           },  // 0x0010
    {0,     0,  NULL,                           },  // 0x0011
    {0,     0,  NULL,                           },  // 0x0012
    {0,     0,  NULL,                           },  // 0x0013
    {0,     0,  NULL,                           },  // 0x0014
    {0,     0,  NULL,                           },  // 0x0015
    {0,     0,  NULL,                           },  // 0x0016
    {0,     0,  NULL,                           },  // 0x0017
    {0,     0,  NULL,                           },  // 0x0018
    {0,     0,  NULL,                           },  // 0x0019
    {0,     0,  NULL,                           },  // 0x001a
    {0,     0,  NULL,                           },  // 0x001b
    {0,     0,  NULL,                           },  // 0x001c
    {0,     0,  NULL,                           },  // 0x001d
    {0,     0,  NULL,                           },  // 0x001e
    {0,     0,  NULL,                           },  // 0x001f
    {0,     0,  NULL,                           },  // 0x0020
    {0,     0,  NULL,                           },  // 0x0021
    {0,     0,  NULL,                           },  // 0x0022
    {0,     0,  NULL,                           },  // 0x0023
    {0,     0,  NULL,                           },  // 0x0024
    {0,     0,  NULL,                           },  // 0x0025
    {0,     0,  NULL,                           },  // 0x0026
    {0,     0,  NULL,                           },  // 0x0027
    {0,     0,  NULL,                           },  // 0x0028
    {0,     0,  NULL,                           },  // 0x0029
    {0,     0,  NULL,                           },  // 0x002a
    {0,     0,  NULL,                           },  // 0x002b
    {0,     0,  NULL,                           },  // 0x002c
    {0,     0,  NULL,                           },  // 0x002d
    {0,     0,  NULL,                           },  // 0x002e
    {0,     0,  NULL,                           },  // 0x002f
    {0,     0,  NULL,                           },  // 0x0030
    {0,     0,  NULL,                           },  // 0x0031
    {0,     0,  NULL,                           },  // 0x0032
    {0,     0,  NULL,                           },  // 0x0033
    {0,     0,  NULL,                           },  // 0x0034
    {0,     0,  NULL,                           },  // 0x0035
    {0,     0,  NULL,                           },  // 0x0036
    {0,     0,  NULL,                           },  // 0x0037
    {0,     0,  NULL,                           },  // 0x0038
    {0,     0,  NULL,                           },  // 0x0039
    {0,     0,  NULL,                           },  // 0x003a
    {0,     0,  NULL,                           },  // 0x003b
    {0,     0,  NULL,                           },  // 0x003c
    {0,     0,  NULL,                           },  // 0x003d
    {0,     0,  NULL,                           },  // 0x003e
    {0,     0,  NULL,                           },  // 0x003f
    {0,     0,  NULL,                           },  // 0x0040
    {0,     0,  NULL,                           },  // 0x0041
    {0,     0,  NULL,                           },  // 0x0042
    {0,     0,  NULL,                           },  // 0x0043
    {0,     0,  NULL,                           },  // 0x0044
    {0,     0,  NULL,                           },  // 0x0045
    {0,     0,  NULL,                           },  // 0x0046
    {0,     0,  NULL,                           },  // 0x0047
    {0,     0,  NULL,                           },  // 0x0048
    {0,     0,  NULL,                           },  // 0x0049
    {0,     0,  NULL,                           },  // 0x004a
    {0,     0,  NULL,                           },  // 0x004b
    {0,     0,  NULL,                           },  // 0x004c
    {0,     0,  NULL,                           },  // 0x004d
    {0,     0,  NULL,                           },  // 0x004e
    {0,     0,  NULL,                           },  // 0x004f
    {0,     0,  NULL,                           },  // 0x0050
    {0,     0,  NULL,                           },  // 0x0051
    {0,     0,  NULL,                           },  // 0x0052
    {0,     0,  NULL,                           },  // 0x0053
    {0,     0,  NULL,                           },  // 0x0054
    {0,     0,  NULL,                           },  // 0x0055
    {0,     0,  NULL,                           },  // 0x0056
    {0,     0,  NULL,                           },  // 0x0057
    {0,     0,  NULL,                           },  // 0x0058
    {0,     0,  NULL,                           },  // 0x0059
    {0,     0,  NULL,                           },  // 0x005a
    {0,     0,  NULL,                           },  // 0x005b
    {0,     0,  NULL,                           },  // 0x005c
    {0,     0,  NULL,                           },  // 0x005d
    {0,     0,  NULL,                           },  // 0x005e
    {0,     0,  NULL,                           },  // 0x005f
    {0,     0,  NULL,                           },  // 0x0060
    {0,     0,  NULL,                           },  // 0x0061
    {0,     0,  NULL,                           },  // 0x0062
    {0,     VAR,NULL,                           },  // 0x0063
    {0,     55, NULL,                           },  // 0x0064
    {0,     17, NULL,                           },  // 0x0065
    {0,     3,  NULL,                           },  // 0x0066
    {0,     37, NULL,                           },  // 0x0067
    {0,     46, NULL,                           },  // 0x0068
    {0,     VAR,NULL,                           },  // 0x0069
    {0,     23, NULL,                           },  // 0x006a
    {0,     VAR,NULL,                           },  // 0x006b
    {0,     3,  NULL,                           },  // 0x006c
    {0,     108,NULL,                           },  // 0x006d
    {0,     3,  NULL,                           },  // 0x006e
    {0,     2,  NULL,                           },  // 0x006f
    {0,     3,  NULL,                           },  // 0x0070
    {0,     28, NULL,                           },  // 0x0071
    {0,     19, clif_parse_WantToConnection,    },  // 0x0072
    {0,     11, NULL,                           },  // 0x0073
    {0,     3,  NULL,                           },  // 0x0074
    {0,     VAR,NULL,                           },  // 0x0075
    {0,     9,  NULL,                           },  // 0x0076
    {0,     5,  NULL,                           },  // 0x0077
    {0,     54, NULL,                           },  // 0x0078
    {0,     53, NULL,                           },  // 0x0079
    {0,     58, NULL,                           },  // 0x007a
    {0,     60, NULL,                           },  // 0x007b
    {0,     41, NULL,                           },  // 0x007c
    {-1,    2,  clif_parse_LoadEndAck,          },  // 0x007d
    {0,     6,  clif_parse_TickSend,            },  // 0x007e
    {0,     6,  NULL,                           },  // 0x007f
    {0,     7,  NULL,                           },  // 0x0080
    {0,     3,  NULL,                           },  // 0x0081
    {0,     2,  NULL,                           },  // 0x0082
    {0,     2,  NULL,                           },  // 0x0083
    {0,     2,  NULL,                           },  // 0x0084
    {-1,    5,  clif_parse_WalkToXY,            },  // 0x0085 Walk code limits this on it's own
    {0,     16, NULL,                           },  // 0x0086
    {0,     12, NULL,                           },  // 0x0087
    {0,     10, NULL,                           },  // 0x0088
    {1000,  7,  clif_parse_ActionRequest,       },  // 0x0089 Special case - see below
    {0,     29, NULL,                           },  // 0x008a
    {0,     23, NULL,                           },  // 0x008b unknown... size 2 or 23?
    {300,   VAR,clif_parse_GlobalMessage,       },  // 0x008c
    {0,     VAR,NULL,                           },  // 0x008d
    {0,     VAR,NULL,                           },  // 0x008e
    {0,     0,  NULL,                           },  // 0x008f
    {500,   7,  clif_parse_NpcClicked,          },  // 0x0090
    {0,     22, NULL,                           },  // 0x0091
    {0,     28, NULL,                           },  // 0x0092
    {0,     2,  NULL,                           },  // 0x0093
    {-1,    6,  clif_parse_GetCharNameRequest,  },  // 0x0094
    {0,     30, NULL,                           },  // 0x0095
    {300,   VAR,clif_parse_Wis,                 },  // 0x0096
    {0,     VAR,NULL,                           },  // 0x0097
    {0,     3,  NULL,                           },  // 0x0098
    {300,   VAR,NULL,                           },  // 0x0099
    {0,     VAR,NULL,                           },  // 0x009a
    {-1,    5,  clif_parse_ChangeDir,           },  // 0x009b
    {0,     9,  NULL,                           },  // 0x009c
    {0,     17, NULL,                           },  // 0x009d
    {0,     17, NULL,                           },  // 0x009e
    {400,   6,  clif_parse_TakeItem,            },  // 0x009f
    {0,     23, NULL,                           },  // 0x00a0
    {0,     6,  NULL,                           },  // 0x00a1
    {50,    6,  clif_parse_DropItem,            },  // 0x00a2
    {0,     VAR,NULL,                           },  // 0x00a3
    {0,     VAR,NULL,                           },  // 0x00a4
    {0,     VAR,NULL,                           },  // 0x00a5
    {0,     VAR,NULL,                           },  // 0x00a6
    {0,     8,  clif_parse_UseItem,             },  // 0x00a7
    {0,     7,  NULL,                           },  // 0x00a8
    {-1,    6,  clif_parse_EquipItem,           },  // 0x00a9 Special case - outfit window (not implemented yet - needs to allow bursts)
    {0,     7,  NULL,                           },  // 0x00aa
    {-1,    4,  clif_parse_UnequipItem,         },  // 0x00ab Special case - outfit window (not implemented yet - needs to allow bursts)
    {0,     7,  NULL,                           },  // 0x00ac
    {0,     0,  NULL,                           },  // 0x00ad
    {0,     VAR,NULL,                           },  // 0x00ae
    {0,     6,  NULL,                           },  // 0x00af
    {0,     8,  NULL,                           },  // 0x00b0
    {0,     8,  NULL,                           },  // 0x00b1
    {0,     3,  clif_parse_Restart,             },  // 0x00b2
    {0,     3,  NULL,                           },  // 0x00b3
    {0,     VAR,NULL,                           },  // 0x00b4
    {0,     6,  NULL,                           },  // 0x00b5
    {0,     6,  NULL,                           },  // 0x00b6
    {0,     VAR,NULL,                           },  // 0x00b7
    {0,     7,  clif_parse_NpcSelectMenu,       },  // 0x00b8
    {-1,    6,  clif_parse_NpcNextClicked,      },  // 0x00b9
    {0,     2,  NULL,                           },  // 0x00ba
    {-1,    5,  clif_parse_StatusUp,            },  // 0x00bb People click this very quickly
    {0,     6,  NULL,                           },  // 0x00bc
    {0,     44, NULL,                           },  // 0x00bd
    {0,     5,  NULL,                           },  // 0x00be
    {1000,  3,  clif_parse_Emotion,             },  // 0x00bf
    {0,     7,  NULL,                           },  // 0x00c0
    {0,     2,  clif_parse_HowManyConnections,  },  // 0x00c1
    {0,     6,  NULL,                           },  // 0x00c2
    {0,     8,  NULL,                           },  // 0x00c3
    {0,     6,  NULL,                           },  // 0x00c4
    {0,     7,  clif_parse_NpcBuySellSelected,  },  // 0x00c5
    {0,     VAR,NULL,                           },  // 0x00c6
    {0,     VAR,NULL,                           },  // 0x00c7
    {-1,    VAR,clif_parse_NpcBuyListSend,      },  // 0x00c8
    {-1,    VAR,clif_parse_NpcSellListSend,     },  // 0x00c9 Selling multiple 1-slot items
    {0,     3,  NULL,                           },  // 0x00ca
    {0,     3,  NULL,                           },  // 0x00cb
    {0,     6,  NULL,                           },  // 0x00cc
    {0,     6,  NULL,                           },  // 0x00cd
    {0,     2,  NULL,                           },  // 0x00ce
    {0,     27, NULL,                           },  // 0x00cf
    {0,     3,  NULL,                           },  // 0x00d0
    {0,     4,  NULL,                           },  // 0x00d1
    {0,     4,  NULL,                           },  // 0x00d2
    {0,     2,  NULL,                           },  // 0x00d3
    {0,     VAR,NULL,                           },  // 0x00d4
    {0,     VAR,NULL,                           },  // 0x00d5
    {0,     3,  NULL,                           },  // 0x00d6
    {0,     VAR,NULL,                           },  // 0x00d7
    {0,     6,  NULL,                           },  // 0x00d8
    {0,     14, NULL,                           },  // 0x00d9
    {0,     3,  NULL,                           },  // 0x00da
    {0,     VAR,NULL,                           },  // 0x00db
    {0,     28, NULL,                           },  // 0x00dc
    {0,     29, NULL,                           },  // 0x00dd
    {0,     VAR,NULL,                           },  // 0x00de
    {0,     VAR,NULL,                           },  // 0x00df
    {0,     30, NULL,                           },  // 0x00e0
    {0,     30, NULL,                           },  // 0x00e1
    {0,     26, NULL,                           },  // 0x00e2
    {0,     2,  NULL,                           },  // 0x00e3
    {2000,  6,  clif_parse_TradeRequest,        },  // 0x00e4
    {0,     26, NULL,                           },  // 0x00e5
    {0,     3,  clif_parse_TradeAck,            },  // 0x00e6
    {0,     3,  NULL,                           },  // 0x00e7
    {0,     8,  clif_parse_TradeAddItem,        },  // 0x00e8
    {0,     19, NULL,                           },  // 0x00e9
    {0,     5,  NULL,                           },  // 0x00ea
    {0,     2,  clif_parse_TradeOk,             },  // 0x00eb
    {0,     3,  NULL,                           },  // 0x00ec
    {0,     2,  clif_parse_TradeCansel,         },  // 0x00ed
    {0,     2,  NULL,                           },  // 0x00ee
    {0,     2,  clif_parse_TradeCommit,         },  // 0x00ef
    {0,     3,  NULL,                           },  // 0x00f0
    {0,     2,  NULL,                           },  // 0x00f1
    {0,     6,  NULL,                           },  // 0x00f2
    {-1,    8,  clif_parse_MoveToKafra,         },  // 0x00f3
    {0,     21, NULL,                           },  // 0x00f4
    {-1,    8,  clif_parse_MoveFromKafra,       },  // 0x00f5
    {0,     8,  NULL,                           },  // 0x00f6
    {0,     2,  clif_parse_CloseKafra,          },  // 0x00f7
    {0,     2,  NULL,                           },  // 0x00f8
    {2000,  26, clif_parse_CreateParty,         },  // 0x00f9
    {0,     3,  NULL,                           },  // 0x00fa
    {0,     VAR,NULL,                           },  // 0x00fb
    {2000,  6,  clif_parse_PartyInvite,         },  // 0x00fc
    {0,     27, NULL,                           },  // 0x00fd
    {0,     30, NULL,                           },  // 0x00fe
    {0,     10, clif_parse_ReplyPartyInvite,    },  // 0x00ff
    {0,     2,  clif_parse_LeaveParty,          },  // 0x0100
    {0,     6,  NULL,                           },  // 0x0101
    {0,     6,  clif_parse_PartyChangeOption,   },  // 0x0102
    {0,     30, clif_parse_RemovePartyMember,   },  // 0x0103
    {0,     79, NULL,                           },  // 0x0104
    {0,     31, NULL,                           },  // 0x0105
    {0,     10, NULL,                           },  // 0x0106
    {0,     10, NULL,                           },  // 0x0107
    {300,   VAR,clif_parse_PartyMessage,        },  // 0x0108
    {0,     VAR,NULL,                           },  // 0x0109
    {0,     4,  NULL,                           },  // 0x010a
    {0,     6,  NULL,                           },  // 0x010b
    {0,     6,  NULL,                           },  // 0x010c
    {0,     2,  NULL,                           },  // 0x010d
    {0,     11, NULL,                           },  // 0x010e
    {0,     VAR,NULL,                           },  // 0x010f
    {0,     10, NULL,                           },  // 0x0110
    {0,     39, NULL,                           },  // 0x0111
    {-1,    4,  clif_parse_SkillUp,             },  // 0x0112
    {0,     10, NULL,                           },  // 0x0113
    {0,     31, NULL,                           },  // 0x0114
    {0,     35, NULL,                           },  // 0x0115
    {0,     10, NULL,                           },  // 0x0116
    {0,     18, NULL,                           },  // 0x0117
    {0,     2,  clif_parse_StopAttack,          },  // 0x0118
    {0,     13, NULL,                           },  // 0x0119
    {0,     15, NULL,                           },  // 0x011a
    {0,     20, NULL,                           },  // 0x011b
    {0,     68, NULL,                           },  // 0x011c
    {0,     2,  NULL,                           },  // 0x011d
    {0,     3,  NULL,                           },  // 0x011e
    {0,     16, NULL,                           },  // 0x011f
    {0,     6,  NULL,                           },  // 0x0120
    {0,     14, NULL,                           },  // 0x0121
    {0,     VAR,NULL,                           },  // 0x0122
    {0,     VAR,NULL,                           },  // 0x0123
    {0,     21, NULL,                           },  // 0x0124
    {0,     8,  NULL,                           },  // 0x0125
    {0,     8,  NULL,                           },  // 0x0126
    {0,     8,  NULL,                           },  // 0x0127
    {0,     8,  NULL,                           },  // 0x0128
    {0,     8,  NULL,                           },  // 0x0129
    {0,     2,  NULL,                           },  // 0x012a
    {0,     2,  NULL,                           },  // 0x012b
    {0,     3,  NULL,                           },  // 0x012c
    {0,     4,  NULL,                           },  // 0x012d
    {0,     2,  NULL,                           },  // 0x012e
    {0,     VAR,NULL,                           },  // 0x012f
    {0,     6,  NULL,                           },  // 0x0130
    {0,     86, NULL,                           },  // 0x0131
    {0,     6,  NULL,                           },  // 0x0132
    {0,     VAR,NULL,                           },  // 0x0133
    {0,     VAR,NULL,                           },  // 0x0134
    {0,     7,  NULL,                           },  // 0x0135
    {0,     VAR,NULL,                           },  // 0x0136
    {0,     6,  NULL,                           },  // 0x0137
    {0,     3,  NULL,                           },  // 0x0138
    {0,     16, NULL,                           },  // 0x0139
    {0,     4,  NULL,                           },  // 0x013a
    {0,     4,  NULL,                           },  // 0x013b
    {0,     4,  NULL,                           },  // 0x013c
    {0,     6,  NULL,                           },  // 0x013d
    {0,     24, NULL,                           },  // 0x013e
    {0,     26, NULL,                           },  // 0x013f
    {0,     22, NULL,                           },  // 0x0140
    {0,     14, NULL,                           },  // 0x0141
    {0,     6,  NULL,                           },  // 0x0142
    {300,   10, clif_parse_NpcAmountInput,      },  // 0x0143
    {0,     23, NULL,                           },  // 0x0144
    {0,     19, NULL,                           },  // 0x0145
    {300,   6,  clif_parse_NpcCloseClicked,     },  // 0x0146
    {0,     39, NULL,                           },  // 0x0147
    {0,     8,  NULL,                           },  // 0x0148
    {0,     9,  NULL,                           },  // 0x0149
    {0,     6,  NULL,                           },  // 0x014a
    {0,     27, NULL,                           },  // 0x014b
    {0,     VAR,NULL,                           },  // 0x014c
    {0,     2,  NULL,                           },  // 0x014d
    {0,     6,  NULL,                           },  // 0x014e
    {0,     6,  NULL,                           },  // 0x014f
    {0,     110,NULL,                           },  // 0x0150
    {0,     6,  NULL,                           },  // 0x0151
    {0,     VAR,NULL,                           },  // 0x0152
    {0,     VAR,NULL,                           },  // 0x0153
    {0,     VAR,NULL,                           },  // 0x0154
    {0,     VAR,NULL,                           },  // 0x0155
    {0,     VAR,NULL,                           },  // 0x0156
    {0,     6,  NULL,                           },  // 0x0157
    {0,     VAR,NULL,                           },  // 0x0158
    {0,     54, NULL,                           },  // 0x0159
    {0,     66, NULL,                           },  // 0x015a
    {0,     54, NULL,                           },  // 0x015b
    {0,     90, NULL,                           },  // 0x015c
    {0,     42, NULL,                           },  // 0x015d
    {0,     6,  NULL,                           },  // 0x015e
    {0,     42, NULL,                           },  // 0x015f
    {0,     VAR,NULL,                           },  // 0x0160
    {0,     VAR,NULL,                           },  // 0x0161
    {0,     VAR,NULL,                           },  // 0x0162
    {0,     VAR,NULL,                           },  // 0x0163
    {0,     VAR,NULL,                           },  // 0x0164
    {0,     30, NULL,                           },  // 0x0165
    {0,     VAR,NULL,                           },  // 0x0166
    {0,     3,  NULL,                           },  // 0x0167
    {0,     14, NULL,                           },  // 0x0168
    {0,     3,  NULL,                           },  // 0x0169
    {0,     30, NULL,                           },  // 0x016a
    {0,     10, NULL,                           },  // 0x016b
    {0,     43, NULL,                           },  // 0x016c
    {0,     14, NULL,                           },  // 0x016d
    {0,     186,NULL,                           },  // 0x016e
    {0,     182,NULL,                           },  // 0x016f
    {0,     14, NULL,                           },  // 0x0170
    {0,     30, NULL,                           },  // 0x0171
    {0,     10, NULL,                           },  // 0x0172
    {0,     3,  NULL,                           },  // 0x0173
    {0,     VAR,NULL,                           },  // 0x0174
    {0,     6,  NULL,                           },  // 0x0175
    {0,     106,NULL,                           },  // 0x0176
    {0,     VAR,NULL,                           },  // 0x0177
    {0,     4,  NULL,                           },  // 0x0178
    {0,     5,  NULL,                           },  // 0x0179
    {0,     4,  NULL,                           },  // 0x017a
    {0,     VAR,NULL,                           },  // 0x017b
    {0,     6,  NULL,                           },  // 0x017c
    {0,     7,  NULL,                           },  // 0x017d
    {0,     VAR,NULL,                           },  // 0x017e
    {0,     VAR,NULL,                           },  // 0x017f
    {0,     6,  NULL,                           },  // 0x0180
    {0,     3,  NULL,                           },  // 0x0181
    {0,     106,NULL,                           },  // 0x0182
    {0,     10, NULL,                           },  // 0x0183
    {0,     10, NULL,                           },  // 0x0184
    {0,     34, NULL,                           },  // 0x0185
    {0,     0,  NULL,                           },  // 0x0186
    {0,     6,  NULL,                           },  // 0x0187
    {0,     8,  NULL,                           },  // 0x0188
    {0,     4,  NULL,                           },  // 0x0189
    {0,     4,  clif_parse_QuitGame,            },  // 0x018a
    {0,     4,  NULL,                           },  // 0x018b
    {0,     29, NULL,                           },  // 0x018c
    {0,     VAR,NULL,                           },  // 0x018d
    {0,     10, NULL,                           },  // 0x018e
    {0,     6,  NULL,                           },  // 0x018f
    {0,     90, NULL,                           },  // 0x0190
    {0,     86, NULL,                           },  // 0x0191
    {0,     24, NULL,                           },  // 0x0192
    {0,     6,  NULL,                           },  // 0x0193
    {0,     30, NULL,                           },  // 0x0194
    {0,     102,NULL,                           },  // 0x0195
    {0,     9,  NULL,                           },  // 0x0196
    {0,     4,  NULL,                           },  // 0x0197
    {0,     8,  NULL,                           },  // 0x0198
    {0,     4,  NULL,                           },  // 0x0199
    {0,     14, NULL,                           },  // 0x019a
    {0,     10, NULL,                           },  // 0x019b
    {0,     VAR,NULL,                           },  // 0x019c
    {300,   6,  NULL,                           },  // 0x019d
    {0,     2,  NULL,                           },  // 0x019e
    {0,     6,  NULL,                           },  // 0x019f
    {0,     3,  NULL,                           },  // 0x01a0
    {0,     3,  NULL,                           },  // 0x01a1
    {0,     35, NULL,                           },  // 0x01a2
    {0,     5,  NULL,                           },  // 0x01a3
    {0,     11, NULL,                           },  // 0x01a4
    {0,     26, NULL,                           },  // 0x01a5
    {0,     VAR,NULL,                           },  // 0x01a6
    {0,     4,  NULL,                           },  // 0x01a7
    {0,     4,  NULL,                           },  // 0x01a8
    {0,     6,  NULL,                           },  // 0x01a9
    {0,     10, NULL,                           },  // 0x01aa
    {0,     12, NULL,                           },  // 0x01ab
    {0,     6,  NULL,                           },  // 0x01ac
    {0,     VAR,NULL,                           },  // 0x01ad
    {0,     4,  NULL,                           },  // 0x01ae
    {0,     4,  NULL,                           },  // 0x01af
    {0,     11, NULL,                           },  // 0x01b0
    {0,     7,  NULL,                           },  // 0x01b1
    {0,     VAR,NULL,                           },  // 0x01b2
    {0,     67, NULL,                           },  // 0x01b3
    {0,     12, NULL,                           },  // 0x01b4
    {0,     18, NULL,                           },  // 0x01b5
    {0,     114,NULL,                           },  // 0x01b6
    {0,     6,  NULL,                           },  // 0x01b7
    {0,     3,  NULL,                           },  // 0x01b8
    {0,     6,  NULL,                           },  // 0x01b9
    {0,     26, NULL,                           },  // 0x01ba
    {0,     26, NULL,                           },  // 0x01bb
    {0,     26, NULL,                           },  // 0x01bc
    {0,     26, NULL,                           },  // 0x01bd
    {0,     2,  NULL,                           },  // 0x01be
    {0,     3,  NULL,                           },  // 0x01bf
    {0,     2,  NULL,                           },  // 0x01c0
    {0,     14, NULL,                           },  // 0x01c1
    {0,     10, NULL,                           },  // 0x01c2
    {0,     VAR,NULL,                           },  // 0x01c3
    {0,     22, NULL,                           },  // 0x01c4
    {0,     22, NULL,                           },  // 0x01c5
    {0,     4,  NULL,                           },  // 0x01c6
    {0,     2,  NULL,                           },  // 0x01c7
    {0,     13, NULL,                           },  // 0x01c8
    {0,     97, NULL,                           },  // 0x01c9
    {0,     0,  NULL,                           },  // 0x01ca
    {0,     9,  NULL,                           },  // 0x01cb
    {0,     9,  NULL,                           },  // 0x01cc
    {0,     30, NULL,                           },  // 0x01cd
    {0,     6,  NULL,                           },  // 0x01ce
    {0,     28, NULL,                           },  // 0x01cf
    {0,     8,  NULL,                           },  // 0x01d0
    {0,     14, NULL,                           },  // 0x01d1
    {0,     10, NULL,                           },  // 0x01d2
    {0,     35, NULL,                           },  // 0x01d3
    {0,     6,  NULL,                           },  // 0x01d4
    {300,   VAR,clif_parse_NpcStringInput,      },  // 0x01d5 - set to -1
    {0,     4,  NULL,                           },  // 0x01d6
    {0,     11, NULL,                           },  // 0x01d7
    {0,     54, NULL,                           },  // 0x01d8
    {0,     53, NULL,                           },  // 0x01d9
    {0,     60, NULL,                           },  // 0x01da
    {0,     2,  NULL,                           },  // 0x01db
    {0,     VAR,NULL,                           },  // 0x01dc
    {0,     47, NULL,                           },  // 0x01dd
    {0,     33, NULL,                           },  // 0x01de
    {0,     6,  NULL,                           },  // 0x01df
    {0,     30, NULL,                           },  // 0x01e0
    {0,     8,  NULL,                           },  // 0x01e1
    {0,     34, NULL,                           },  // 0x01e2
    {0,     14, NULL,                           },  // 0x01e3
    {0,     2,  NULL,                           },  // 0x01e4
    {0,     6,  NULL,                           },  // 0x01e5
    {0,     26, NULL,                           },  // 0x01e6
    {0,     2,  NULL,                           },  // 0x01e7
    {0,     28, NULL,                           },  // 0x01e8
    {0,     81, NULL,                           },  // 0x01e9
    {0,     6,  NULL,                           },  // 0x01ea
    {0,     10, NULL,                           },  // 0x01eb
    {0,     26, NULL,                           },  // 0x01ec
    {0,     2,  NULL,                           },  // 0x01ed
    {0,     VAR,NULL,                           },  // 0x01ee
    {0,     VAR,NULL,                           },  // 0x01ef
    {0,     VAR,NULL,                           },  // 0x01f0
    {0,     VAR,NULL,                           },  // 0x01f1
    {0,     20, NULL,                           },  // 0x01f2
    {0,     10, NULL,                           },  // 0x01f3
    {0,     32, NULL,                           },  // 0x01f4
    {0,     9,  NULL,                           },  // 0x01f5
    {0,     34, NULL,                           },  // 0x01f6
    {0,     14, NULL,                           },  // 0x01f7
    {0,     2,  NULL,                           },  // 0x01f8
    {0,     6,  NULL,                           },  // 0x01f9
    {0,     48, NULL,                           },  // 0x01fa
    {0,     56, NULL,                           },  // 0x01fb
    {0,     VAR,NULL,                           },  // 0x01fc
    {0,     4,  NULL,                           },  // 0x01fd
    {0,     5,  NULL,                           },  // 0x01fe
    {0,     10, NULL,                           },  // 0x01ff
    {0,     26, NULL,                           },  // 0x0200
    {0,     VAR,NULL,                           },  // 0x0201
    {0,     26, NULL,                           },  // 0x0202
    {0,     10, NULL,                           },  // 0x0203
    {0,     18, NULL,                           },  // 0x0204
    {0,     26, NULL,                           },  // 0x0205
    {0,     11, NULL,                           },  // 0x0206
    {0,     34, NULL,                           },  // 0x0207
    {0,     14, NULL,                           },  // 0x0208
    {0,     36, NULL,                           },  // 0x0209
    {0,     10, NULL,                           },  // 0x020a
    {0,     19, NULL,                           },  // 0x020b
    {0,     10, NULL,                           },  // 0x020c
    {0,     VAR,NULL,                           },  // 0x020d
    {0,     24, NULL,                           },  // 0x020e
    {0,     0,  NULL,                           },  // 0x020f
    {0,     0,  NULL,                           },  // 0x0210
    {0,     0,  NULL,                           },  // 0x0211
    {0,     0,  NULL,                           },  // 0x0212
    {0,     0,  NULL,                           },  // 0x0213
    {0,     0,  NULL,                           },  // 0x0214
    {0,     0,  NULL,                           },  // 0x0215
    {0,     0,  NULL,                           },  // 0x0216
    {0,     0,  NULL,                           },  // 0x0217
    {0,     0,  NULL,                           },  // 0x0218
    {0,     0,  NULL,                           },  // 0x0219
    {0,     0,  NULL,                           },  // 0x021a
    {0,     0,  NULL,                           },  // 0x021b
    {0,     0,  NULL,                           },  // 0x021c
    {0,     0,  NULL,                           },  // 0x021d
    {0,     0,  NULL,                           },  // 0x021e
    {0,     0,  NULL,                           },  // 0x021f
};

// Checks for packet flooding
static
int clif_check_packet_flood(Session *s, int cmd)
{
    dumb_ptr<map_session_data> sd = dumb_ptr<map_session_data>(static_cast<map_session_data *>(s->session_data.get()));
    tick_t tick = gettick();

    // sd will not be set if the client hasn't requested
    // WantToConnection yet. Do not apply flood logic to GMs
    // as approved bots (GMlvl1) should not have to work around
    // flood logic.
    if (!sd || pc_isGM(sd) || clif_parse_func_table[cmd].rate == static_cast<interval_t>(-1))
        return 0;

    // Timer has wrapped
    if (tick < sd->flood_rates[cmd])
    {
        sd->flood_rates[cmd] = tick;
        return 0;
    }

    // Default rate is 100ms
    interval_t rate = clif_parse_func_table[cmd].rate;
    if (rate == interval_t::zero())
        rate = std::chrono::milliseconds(100);

    // ActionRequest - attacks are allowed a faster rate than sit/stand
    if (cmd == 0x89)
    {
        int action_type = RFIFOB(s, 6);
        if (action_type == 0x00 || action_type == 0x07)
            rate = std::chrono::milliseconds(20);
        else
            rate = std::chrono::seconds(1);
    }

// Restore this code when mana1.0 is released
#if 0
    // ChangeDir - only apply limit if not walking
    if (cmd == 0x9b)
    {
        // .29 clients spam this packet when walking into a blocked tile
        if (RFIFOB(fd, 4) == sd->dir || sd->walktimer != -1)
            return 0;

        rate = 500;
    }
#endif

    // They are flooding
    if (tick < sd->flood_rates[cmd] + rate)
    {
        TimeT now = TimeT::now();

        // If it's a nasty flood we log and possibly kick
        if (now > sd->packet_flood_reset_due)
        {
            sd->packet_flood_reset_due = static_cast<time_t>(now) + battle_config.packet_spam_threshold;
            sd->packet_flood_in = 0;
        }

        sd->packet_flood_in++;

        if (sd->packet_flood_in >= battle_config.packet_spam_flood)
        {
            PRINTF("packet flood detected from %s [0x%x]\n"_fmt, sd->status_key.name, cmd);
            if (battle_config.packet_spam_kick)
            {
                s->set_eof();
                return 1;
            }
            sd->packet_flood_in = 0;
        }

        return 1;
    }

    sd->flood_rates[cmd] = tick;
    return 0;
}

inline
void WARN_MALFORMED_MSG(dumb_ptr<map_session_data> sd, ZString msg)
{
    PRINTF("clif_validate_chat(): %s (ID %d) sent a malformed message: %s.\n"_fmt,
            sd->status_key.name, sd->status_key.account_id, msg);
}
/**
 * Validate message integrity (inspired by upstream source (eAthena)).
 *
 * @param sd active session data
 * @param type message type:
 *  0 for when the sender's name is not included (party chat)
 *  1 for when the target's name is included (whisper chat)
 *  2 for when the sender's name is given ("sender : text", public/guild chat)
 * @return a dynamically allocated copy of the message, or empty string upon failure
 */
static
AString clif_validate_chat(dumb_ptr<map_session_data> sd, ChatType type)
{
    nullpo_retr(AString(), sd);
    /*
     * Don't send chat in the period between the ban and the connection's
     * closure.
     */
    if (sd->auto_ban_info.in_progress)
        return AString();

    Session *s = sd->sess;
    size_t msg_len = RFIFOW(s, 2) - 4;
    size_t name_len = sd->status_key.name.to__actual().size();
    /*
     * At least one character is required in all instances.
     * Notes for length checks:
     *
     * For all types, header (2) + length (2) is considered empty.
     * For type 1, the message must be longer than the maximum name length (24)
     *      to be valid.
     * For type 2, the message must be longer than the sender's name length
     *      plus the length of the separator (" : ").
     */
    size_t min_len =
        (type == ChatType::Whisper) ? 24
        : (type == ChatType::Global) ? name_len + 3
        : 0;

    /* The player just sent the header (2) and length (2) words. */
    if (!msg_len)
    {
        WARN_MALFORMED_MSG(sd, "no message sent"_s);
        return AString();
    }

    /* The client sent (or claims to have sent) an empty message. */
    if (msg_len == min_len)
    {
        WARN_MALFORMED_MSG(sd, "empty message"_s);
        return AString();
    }

    /* The protocol specifies that the target must be 24 bytes long. */
    if (type == ChatType::Whisper && msg_len < min_len)
    {
        /* Disallow malformed messages. */
        clif_setwaitclose(s);
        WARN_MALFORMED_MSG(sd, "illegal target name"_s);
        return AString();
    }

    size_t pstart = 4;
    size_t buf_len = msg_len;
    if (type == ChatType::Whisper)
    {
        pstart += 24;
        buf_len -= 24;
    }
    AString pbuf = RFIFO_STRING(s, pstart, buf_len);

    /*
     * The client attempted to exceed the maximum message length.
     *
     * The conf suggests up to chat_maxline characters, after which the message
     * is truncated. But the previous behavior was to drop the message, so
     * we'll do that, too.
     */
    if (buf_len >= battle_config.chat_maxline)
    {
        WARN_MALFORMED_MSG(sd, "exceeded maximum message length"_s);
        return AString();
    }

    if (type == ChatType::Global)
    {
        XString p = pbuf;
        if (!(p.startswith(sd->status_key.name.to__actual()) && p.xslice_t(name_len).startswith(" : "_s)))
        {
            /* Disallow malformed/spoofed messages. */
            clif_setwaitclose(s);
            WARN_MALFORMED_MSG(sd, "spoofed name/invalid format"_s);
            return AString();
        }
        /* Step beyond the separator. */
        XString xs = p.xslice_t(name_len + 3);
        return xs;
    }
    return pbuf;
}

/*==========================================
 * クライアントからのパケット解析
 * socket.cのdo_parsepacketから呼び出される
 *------------------------------------------
 */
static
void clif_parse(Session *s)
{
    int packet_len = 0, cmd = 0;
    dumb_ptr<map_session_data> sd = dumb_ptr<map_session_data>(static_cast<map_session_data *>(s->session_data.get()));

    if (!sd || (sd && !sd->state.auth))
    {
        if (RFIFOREST(s) < 2)
        {                       // too small a packet disconnect
            s->set_eof();
        }
        if (RFIFOW(s, 0) != 0x72 && RFIFOW(s, 0) != 0x7530)
        {
            // first packet must be auth or finger
            s->set_eof();
        }
    }

    if (!chrif_isconnect())
    {
        s->set_eof();
        return;
    }

    if (RFIFOREST(s) < 2)
        return;               // Too small (no packet number)

    cmd = RFIFOW(s, 0);

    // 管理用パケット処理
    if (cmd >= 30000)
    {
        switch (cmd)
        {
            case 0x7530:       // Athena情報所得
                WFIFOW(s, 0) = 0x7531;
                WFIFO_STRUCT(s, 2, CURRENT_MAP_SERVER_VERSION);
                WFIFOSET(s, 10);
                RFIFOSKIP(s, 2);
                break;
            case 0x7532:       // 接続の切断
                s->set_eof();
                break;
        }
        return;
    }
    else if (cmd >= 0x200)
        return;

    // パケット長を計算
    packet_len = clif_parse_func_table[cmd].len;
    if (packet_len == VAR)
    {
        if (RFIFOREST(s) < 4)
        {
            return;           // Runt packet (variable length without a length sent)
        }
        packet_len = RFIFOW(s, 2);
        if (packet_len < 4 || packet_len > 32768)
        {
            s->set_eof();
            return;           // Runt packet (variable out of bounds)
        }
    }

    if (RFIFOREST(s) < packet_len)
    {
        return;               // Runt packet (sent legnth is too small)
    }

    if (sd && sd->state.auth == 1 && sd->state.waitingdisconnect == 1)
    {                           // 切断待ちの場合パケットを処理しない

    }
    else if (clif_parse_func_table[cmd].func)
    {
        if (clif_check_packet_flood(s, cmd))
        {
            // Flood triggered. Skip packet.
            RFIFOSKIP(sd->sess, packet_len);
            return;
        }

        clif_parse_func_table[cmd].func(s, sd);
    }
    else
    {
        // 不明なパケット
        if (battle_config.error_log)
        {
            if (s)
                PRINTF("\nclif_parse: session #%d, packet 0x%x, lenght %d\n"_fmt,
                        s, cmd, packet_len);
#ifdef DUMP_UNKNOWN_PACKET
            {
                int i;
                ZString packet_txt = "save/packet.txt"_s;
                PRINTF("---- 00-01-02-03-04-05-06-07-08-09-0A-0B-0C-0D-0E-0F"_fmt);
                for (i = 0; i < packet_len; i++)
                {
                    if ((i & 15) == 0)
                        PRINTF("\n%04X "_fmt, i);
                    PRINTF("%02X "_fmt, RFIFOB(s, i));
                }
                if (sd && sd->state.auth)
                {
                    PRINTF("\nAccount ID %d, character ID %d, player name %s.\n"_fmt,
                            sd->status_key.account_id, sd->status_key.char_id,
                            sd->status_key.name);
                }
                else if (sd)    // not authentified! (refused by char-server or disconnect before to be authentified)
                    PRINTF("\nAccount ID %d.\n"_fmt, sd->bl_id);

                io::AppendFile fp(packet_txt);
                if (!fp.is_open())
                {
                    PRINTF("clif.c: cant write [%s] !!! data is lost !!!\n"_fmt,
                            packet_txt);
                    return;
                }
                else
                {
                    timestamp_seconds_buffer now;
                    stamp_time(now);
                    if (sd && sd->state.auth)
                    {
                        FPRINTF(fp,
                                "%s\nPlayer with account ID %d (character ID %d, player name %s) sent wrong packet:\n"_fmt,
                                now,
                                sd->status_key.account_id,
                                sd->status_key.char_id, sd->status_key.name);
                    }
                    else if (sd)    // not authentified! (refused by char-server or disconnect before to be authentified)
                        FPRINTF(fp,
                                "%s\nPlayer with account ID %d sent wrong packet:\n"_fmt,
                                now, sd->bl_id);

                    FPRINTF(fp,
                             "\t---- 00-01-02-03-04-05-06-07-08-09-0A-0B-0C-0D-0E-0F"_fmt);
                    for (i = 0; i < packet_len; i++)
                    {
                        if ((i & 15) == 0)
                            FPRINTF(fp, "\n\t%04X "_fmt, i);
                        FPRINTF(fp, "%02X "_fmt, RFIFOB(s, i));
                    }
                    FPRINTF(fp, "\n\n"_fmt);
                }
            }
#endif
        }
    }
    RFIFOSKIP(s, packet_len);
}

void do_init_clif(void)
{
    make_listen_port(map_port, SessionParsers{func_parse: clif_parse, func_delete: clif_delete});
}
