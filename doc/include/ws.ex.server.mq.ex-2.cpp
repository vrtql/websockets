// Subscribe to "news" topic
vrtql_msg* sub = vrtql_msg_new();
vrtql_msg_set_routing(sub, "action", "subscribe");
vrtql_msg_set_routing(sub, "topic", "news");
vrtql_msg_send(cnx, sub);
vrtql_msg_free(sub);

// Publish a message to "news" topic
vrtql_msg* pub = vrtql_msg_new();
vrtql_msg_set_routing(pub, "action", "publish");
vrtql_msg_set_routing(pub, "topic", "news");
vrtql_msg_set_content(pub, "Breaking news: VRTQL 2.0 released!");
vrtql_msg_send(cnx, pub);
vrtql_msg_free(pub);
