#include <string.h>
#include <vws/server.h>
#include <vws/message.h>

//----------------------------------------------------------------------
// Subscription storage
//----------------------------------------------------------------------

// Maximum topics and subscribers per topic
#define MAX_TOPICS 64
#define MAX_SUBS   256

typedef struct
{
    char topic[128];
    vws_cid_t subscribers[MAX_SUBS];
    int count;
} topic_entry;

typedef struct
{
    topic_entry topics[MAX_TOPICS];
    int count;
    uv_mutex_t lock;
} broker_state;

static broker_state broker;

void broker_init()
{
    memset(&broker, 0, sizeof(broker));
    uv_mutex_init(&broker.lock);
}

void broker_destroy()
{
    uv_mutex_destroy(&broker.lock);
}

// Find or create a topic entry
topic_entry* broker_get_topic(cstr topic, bool create)
{
    for (int i = 0; i < broker.count; i++)
    {
        if (strcmp(broker.topics[i].topic, topic) == 0)
        {
            return &broker.topics[i];
        }
    }

    if (create && broker.count < MAX_TOPICS)
    {
        topic_entry* e = &broker.topics[broker.count++];
        strncpy(e->topic, topic, sizeof(e->topic) - 1);
        e->count = 0;
        return e;
    }

    return NULL;
}

void broker_subscribe(cstr topic, vws_cid_t cid)
{
    uv_mutex_lock(&broker.lock);

    topic_entry* e = broker_get_topic(topic, true);

    if (e != NULL && e->count < MAX_SUBS)
    {
        e->subscribers[e->count++] = cid;
    }

    uv_mutex_unlock(&broker.lock);
}

void broker_unsubscribe(cstr topic, vws_cid_t cid)
{
    uv_mutex_lock(&broker.lock);

    topic_entry* e = broker_get_topic(topic, false);

    if (e != NULL)
    {
        for (int i = 0; i < e->count; i++)
        {
            if (e->subscribers[i].key == cid.key)
            {
                // Shift remaining subscribers down
                for (int j = i; j < e->count - 1; j++)
                {
                    e->subscribers[j] = e->subscribers[j + 1];
                }

                e->count--;
                break;
            }
        }
    }

    uv_mutex_unlock(&broker.lock);
}

//----------------------------------------------------------------------
// Message processing
//----------------------------------------------------------------------

void process(vws_svr* s, vws_cid_t cid, vrtql_msg* m, void* ctx)
{
    vrtql_msg_svr* server = (vrtql_msg_svr*)s;

    cstr action = vrtql_msg_get_routing(m, "action");
    cstr topic  = vrtql_msg_get_routing(m, "topic");

    if (action == NULL || topic == NULL)
    {
        vrtql_msg_free(m);
        return;
    }

    if (strcmp(action, "subscribe") == 0)
    {
        broker_subscribe(topic, cid);

        // Acknowledge
        vrtql_msg* ack = vrtql_msg_new();
        ack->format    = m->format;
        vrtql_msg_set_routing(ack, "action", "subscribed");
        vrtql_msg_set_routing(ack, "topic", topic);
        server->send(s, cid, ack, NULL);
    }
    else if (strcmp(action, "unsubscribe") == 0)
    {
        broker_unsubscribe(topic, cid);

        // Acknowledge
        vrtql_msg* ack = vrtql_msg_new();
        ack->format    = m->format;
        vrtql_msg_set_routing(ack, "action", "unsubscribed");
        vrtql_msg_set_routing(ack, "topic", topic);
        server->send(s, cid, ack, NULL);
    }
    else if (strcmp(action, "publish") == 0)
    {
        uv_mutex_lock(&broker.lock);

        topic_entry* e = broker_get_topic(topic, false);

        if (e != NULL)
        {
            for (int i = 0; i < e->count; i++)
            {
                // Create a copy of the message for each subscriber
                vrtql_msg* copy = vrtql_msg_new();
                copy->format    = m->format;

                vrtql_msg_set_routing(copy, "action", "message");
                vrtql_msg_set_routing(copy, "topic", topic);

                if (m->content->size > 0)
                {
                    vws_buffer_append( copy->content,
                                       m->content->data,
                                       m->content->size );
                }

                // Use dispatch() to send without freeing
                server->dispatch(s, e->subscribers[i], copy, NULL);
            }
        }

        uv_mutex_unlock(&broker.lock);
    }

    vrtql_msg_free(m);
}

int main(int argc, const char* argv[])
{
    broker_init();

    // Create server
    vrtql_msg_svr* server = vrtql_msg_svr_new(10, 0, 0);
    server->process       = process;

    // Run
    vrtql_msg_svr_run(server, "127.0.0.1", 8181);

    // Cleanup
    vrtql_msg_svr_free(server);
    broker_destroy();
    vws_cleanup();

    return 0;
}
