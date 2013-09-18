#include "proto-http.h"
#include "proto-banner1.h"
#include "smack.h"
#include <ctype.h>
#include <stdint.h>

enum {
    HTTPFIELD_INCOMPLETE,
    HTTPFIELD_SERVER,
    HTTPFIELD_CONTENT_LENGTH,
    HTTPFIELD_CONTENT_TYPE,
    HTTPFIELD_VIA,
    HTTPFIELD_LOCATION,
    HTTPFIELD_UNKNOWN,
    HTTPFIELD_NEWLINE,
};
struct Patterns http_fields[] = {
    {"Server:",          7, HTTPFIELD_SERVER,           SMACK_ANCHOR_BEGIN},
    //{"Content-Length:", 15, HTTPFIELD_CONTENT_LENGTH,   SMACK_ANCHOR_BEGIN},
    //{"Content-Type:",   13, HTTPFIELD_CONTENT_TYPE,     SMACK_ANCHOR_BEGIN},
    {"Via:",             4, HTTPFIELD_VIA,              SMACK_ANCHOR_BEGIN},
    {"Location:",        9, HTTPFIELD_LOCATION,         SMACK_ANCHOR_BEGIN},
    {":",                1, HTTPFIELD_UNKNOWN, 0},
    {"\n",               1, HTTPFIELD_NEWLINE, 0}, 
    {0,0,0,0}
};
enum {
    HTML_INCOMPLETE,
    HTML_TITLE,
    HTML_UNKNOWN,
};
struct Patterns html_fields[] = {
    {"<Title",          6, HTML_TITLE, 0},
    {0,0,0,0}
};


/*****************************************************************************
 *****************************************************************************/
static void
field_name(void *banner, unsigned *banner_offset, size_t banner_max, unsigned id, struct Patterns *http_fields)
{
    unsigned i;
    if (id == HTTPFIELD_INCOMPLETE)
        return;
    if (id == HTTPFIELD_UNKNOWN)
        return;
    if (id == HTTPFIELD_NEWLINE)
        return;
    for (i=0; http_fields[i].pattern; i++) {
        if (http_fields[i].id == id) {
            if (*banner_offset != 0)
                banner_append("\n", 1, banner, banner_offset, banner_max);
            banner_append(http_fields[i].pattern 
                            + ((http_fields[i].pattern[0]=='<')?1:0), /* bah. hack. ugly. */
                          http_fields[i].pattern_length
                            - ((http_fields[i].pattern[0]=='<')?1:0), /* bah. hack. ugly. */
                          banner,
                          banner_offset,
                          banner_max);
            return;
        }
    }
}

/*****************************************************************************
 * Initialize some stuff that's part of the HTTP state-machine-parser.
 *****************************************************************************/
void
http_init(struct Banner1 *b)
{
    unsigned i;
    
    /*
     * These match HTTP Header-Field: names
     */
    b->http_fields = smack_create("http", SMACK_CASE_INSENSITIVE);
    for (i=0; http_fields[i].pattern; i++)
        smack_add_pattern(
                          b->http_fields,
                          http_fields[i].pattern,
                          http_fields[i].pattern_length,
                          http_fields[i].id,
                          http_fields[i].is_anchored);
    smack_compile(b->http_fields);
    
    /*
     * These match HTML <tag names
     */
    b->html_fields = smack_create("html", SMACK_CASE_INSENSITIVE);
    for (i=0; html_fields[i].pattern; i++)
        smack_add_pattern(
                          b->html_fields,
                          html_fields[i].pattern,
                          html_fields[i].pattern_length,
                          html_fields[i].id,
                          html_fields[i].is_anchored);
    smack_compile(b->html_fields);
    
}

/***************************************************************************
 * BIZARRE CODE ALERT!
 *
 * This uses a "byte-by-byte state-machine" to parse the response HTTP
 * header. This is standard practice for high-performance network
 * devices, but is probably unfamiliar to the average network engineer.
 *
 * The way this works is that each byte of input causes a transition to
 * the next state. That means we can parse the response from a server
 * without having to buffer packets. The server can send the response
 * one byte at a time (one packet for each byte) or in one entire packet.
 * Either way, we don't. We don't need to buffer the entire response
 * header waiting for the final packet to arrive, but handle each packet
 * individually.
 * 
 * This is especially useful with our custom TCP stack, which simply
 * rejects out-of-order packets.
 ***************************************************************************/
unsigned
banner_http(  struct Banner1 *banner1,
        unsigned state,
        const unsigned char *px, size_t length,
        char *banner, unsigned *banner_offset, size_t banner_max)
{
    unsigned i;
    unsigned state2;
    size_t id;
    enum {
        FIELD_START = 9,
        FIELD_NAME,
        FIELD_COLON,
        FIELD_VALUE,
        CONTENT,
        CONTENT_TAG,
        CONTENT_FIELD
    };

    state2 = (state>>16) & 0xFFFF;
    id = (state>>8) & 0xFF;
    state = (state>>0) & 0xFF;

    for (i=0; i<length; i++)
    switch (state) {
    case 0: case 1: case 2: case 3: case 4:
        if (toupper(px[i]) != "HTTP/"[state])
            state = STATE_DONE;
        else
            state++;
        break;
    case 5:
        if (px[i] == '.')
            state++;
        else if (!isdigit(px[i]))
            state = STATE_DONE;
        break;
    case 6:
        if (isspace(px[i]))
            state++;
        else if (!isdigit(px[i]))
            state = STATE_DONE;
        break;
    case 7:
        /* TODO: look for 1xx response code */
        if (px[i] == '\n')
            state = FIELD_START;
        break;
    case FIELD_START:
        if (px[i] == '\r')
            break;
        else if (px[i] == '\n') {
            state2 = 0;
            state = CONTENT;
            break;
        } else {
            state2 = 0;
            state = FIELD_NAME;
            /* drop down */
        }

    case FIELD_NAME:
        if (px[i] == '\r')
            break;
        id = smack_search_next(
                        banner1->http_fields,
                        &state2, 
                        px, &i, (unsigned)length);
        i--;
        if (id == HTTPFIELD_NEWLINE) {
            state2 = 0;
            state = FIELD_START;
        } else if (id == SMACK_NOT_FOUND)
            ; /* continue here */
        else if (id == HTTPFIELD_UNKNOWN) {
            /* Oops, at this point, both ":" and "Server:" will match.
             * Therefore, we need to make sure ":" was found, and not
             * a known field like "Server:" */
            size_t id2;

            id2 = smack_next_match(banner1->http_fields, &state2);
            if (id2 != SMACK_NOT_FOUND)
                id = id2;
        
            state = FIELD_COLON;
        } else
            state = FIELD_COLON;
        break;
    case FIELD_COLON:
        if (px[i] == '\n') {
            state = FIELD_START;
            break;
        } else if (isspace(px[i])) {
            break;
        } else {
            field_name(banner, banner_offset, banner_max, id, http_fields);
            state = FIELD_VALUE;
            /* drop down */
        }

    case FIELD_VALUE:
        if (px[i] == '\r')
            break;
        else if (px[i] == '\n') {
            state = FIELD_START;
            break;
        }
        switch (id) {
        case HTTPFIELD_SERVER:
        case HTTPFIELD_LOCATION:
        case HTTPFIELD_VIA:
            banner_append(&px[i], 1, banner, banner_offset, banner_max);
            break;
        case HTTPFIELD_CONTENT_LENGTH:
                if (isdigit(px[i]&0xFF)) {
                    ; /*todo: add content length parsing */
                } else {
                    id = 0;
                }
            break;
        }
        break;
    case CONTENT:
            id = smack_search_next(
                                   banner1->html_fields,
                                   &state2, 
                                   px, &i, (unsigned)length);
            i--;
            if (id != SMACK_NOT_FOUND) {
                field_name(banner, banner_offset, banner_max, id, html_fields);
                banner_append(":", 1, banner, banner_offset, banner_max);
                state = CONTENT_TAG;
            }
            break;
    case CONTENT_TAG:
        for (; i<length; i++) {
            if (px[i] == '>') {
                state = CONTENT_FIELD;
                break;
            }
        }
        break;
    case CONTENT_FIELD:
        if (px[i] == '<')
            state = CONTENT;
        else {
            banner_append(&px[i], 1, banner, banner_offset, banner_max);
        }
        break;
    case STATE_DONE:
    default:
        i = (unsigned)length;
        break;
    }


    if (state == STATE_DONE)
        return state;
    else
        return (state2 & 0xFFFF) << 16
                | (id & 0xFF) << 8
                | (state & 0xFF);
}
