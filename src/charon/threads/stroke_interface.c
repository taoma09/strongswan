/**
 * @file stroke.c
 * 
 * @brief Implementation of stroke_t.
 * 
 */

/*
 * Copyright (C) 2006 Martin Willi
 * Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>

#include "stroke_interface.h"

#include <library.h>
#include <stroke.h>
#include <daemon.h>
#include <crypto/x509.h>
#include <crypto/crl.h>
#include <queues/jobs/initiate_job.h>
#include <queues/jobs/route_job.h>
#include <utils/leak_detective.h>

#define IKE_PORT	500
#define PATH_BUF	256


struct sockaddr_un socket_addr = { AF_UNIX, STROKE_SOCKET};


typedef struct private_stroke_t private_stroke_t;

/**
 * Private data of an stroke_t object.
 */
struct private_stroke_t {

	/**
	 * Public part of stroke_t object.
	 */
	stroke_t public;
	
	/**
	 * Output stream (stroke console)
	 */
	FILE *out;
		
	/**
	 * Unix socket to listen for strokes
	 */
	int socket;
	
	/**
	 * Thread which reads from the Socket
	 */
	pthread_t assigned_thread;
};

/**
 * Helper function which corrects the string pointers
 * in a stroke_msg_t. Strings in a stroke_msg sent over "wire"
 * contains RELATIVE addresses (relative to the beginning of the
 * stroke_msg). They must be corrected if they reach our address
 * space...
 */
static void pop_string(stroke_msg_t *msg, char **string)
{
	if (*string == NULL)
		return;

	/* check for sanity of string pointer and string */
	if (string < (char**)msg
	||	string > (char**)msg + sizeof(stroke_msg_t)
	|| (unsigned long)*string < (unsigned long)((char*)msg->buffer - (char*)msg)
	|| (unsigned long)*string > msg->length)
	{
		*string = "(invalid pointer in stroke msg)";
	}
	else
	{
		*string = (char*)msg + (unsigned long)*string;
	}
}

/**
 * Load end entitity certificate
 */
static x509_t* load_end_certificate(const char *filename, identification_t **idp)
{
	char path[PATH_BUF];
	x509_t *cert;

	if (*filename == '/')
	{
		/* absolute path name */
		snprintf(path, sizeof(path), "%s", filename);
	}
	else
	{
		/* relative path name */
		snprintf(path, sizeof(path), "%s/%s", CERTIFICATE_DIR, filename);
	}

	cert = x509_create_from_file(path, "end entity certificate");

	if (cert)
	{
		identification_t *id = *idp;
		identification_t *subject = cert->get_subject(cert);

		err_t ugh = cert->is_valid(cert, NULL);

		if (ugh != NULL)	
		{
			DBG1(DBG_CFG, "warning: certificate %s", ugh);
		}
		if (!id->equals(id, subject) && !cert->equals_subjectAltName(cert, id))
		{
			id->destroy(id);
			id = subject;
			*idp = id->clone(id);
		}
		return charon->credentials->add_end_certificate(charon->credentials, cert);
	}
	return NULL;
}

/**
 * Add a connection to the configuration list
 */
static void stroke_add_conn(private_stroke_t *this, stroke_msg_t *msg)
{
	connection_t *connection;
	policy_t *policy;
	identification_t *my_id, *other_id;
	identification_t *my_ca = NULL;
	identification_t *other_ca = NULL;
	bool my_ca_same = FALSE;
    bool other_ca_same =FALSE;
	host_t *my_host, *other_host, *my_subnet, *other_subnet;
	proposal_t *proposal;
	traffic_selector_t *my_ts, *other_ts;
	
	pop_string(msg, &msg->add_conn.name);
	pop_string(msg, &msg->add_conn.me.address);
	pop_string(msg, &msg->add_conn.other.address);
	pop_string(msg, &msg->add_conn.me.subnet);
	pop_string(msg, &msg->add_conn.other.subnet);
	pop_string(msg, &msg->add_conn.me.id);
	pop_string(msg, &msg->add_conn.other.id);
	pop_string(msg, &msg->add_conn.me.cert);
	pop_string(msg, &msg->add_conn.other.cert);
	pop_string(msg, &msg->add_conn.me.ca);
	pop_string(msg, &msg->add_conn.other.ca);
	pop_string(msg, &msg->add_conn.me.updown);
	pop_string(msg, &msg->add_conn.other.updown);
	pop_string(msg, &msg->add_conn.algorithms.ike);
	pop_string(msg, &msg->add_conn.algorithms.esp);
	
	DBG1(DBG_CFG, "received stroke: add connection '%s'", msg->add_conn.name);
	
	DBG2(DBG_CFG, "conn %s", msg->add_conn.name);
	DBG2(DBG_CFG, "  right=%s", msg->add_conn.me.address);
	DBG2(DBG_CFG, "  left=%s", msg->add_conn.other.address);
	DBG2(DBG_CFG, "  rightsubnet=%s", msg->add_conn.me.subnet);
	DBG2(DBG_CFG, "  leftsubnet=%s", msg->add_conn.other.subnet);
	DBG2(DBG_CFG, "  rightid=%s", msg->add_conn.me.id);
	DBG2(DBG_CFG, "  leftid=%s", msg->add_conn.other.id);
	DBG2(DBG_CFG, "  rightcert=%s", msg->add_conn.me.cert);
	DBG2(DBG_CFG, "  leftcert=%s", msg->add_conn.other.cert);
	DBG2(DBG_CFG, "  rightca=%s", msg->add_conn.me.ca);
	DBG2(DBG_CFG, "  leftca=%s", msg->add_conn.other.ca);
	DBG2(DBG_CFG, "  ike=%s", msg->add_conn.algorithms.ike);
	DBG2(DBG_CFG, "  esp=%s", msg->add_conn.algorithms.esp);
	
	my_host = msg->add_conn.me.address?
			  host_create_from_string(msg->add_conn.me.address, IKE_PORT) : NULL;
	if (my_host == NULL)
	{
		DBG1(DBG_CFG, "invalid host: %s\n", msg->add_conn.me.address);
		return;
	}

	other_host = msg->add_conn.other.address ?
			host_create_from_string(msg->add_conn.other.address, IKE_PORT) : NULL;
	if (other_host == NULL)
	{
		DBG1(DBG_CFG, "invalid host: %s\n", msg->add_conn.other.address);
		my_host->destroy(my_host);
		return;
	}

	if (charon->socket->is_local_address(charon->socket, other_host, NULL))
	{
		stroke_end_t tmp_end;
		host_t *tmp_host;

		DBG2(DBG_CFG, "left is other host, swapping ends\n");

		tmp_host = my_host;
		my_host = other_host;
		other_host = tmp_host;

		tmp_end = msg->add_conn.me;
		msg->add_conn.me = msg->add_conn.other;
		msg->add_conn.other = tmp_end;
	}
	else if (!charon->socket->is_local_address(charon->socket, my_host, NULL))
	{
		DBG1(DBG_CFG, "left nor right host is our side, aborting\n");
		goto destroy_hosts;
	}

	my_id = identification_create_from_string(msg->add_conn.me.id ?
						msg->add_conn.me.id : msg->add_conn.me.address);
	if (my_id == NULL)
	{
		DBG1(DBG_CFG, "invalid ID: %s\n", msg->add_conn.me.id);
		goto destroy_hosts;
	}

	other_id = identification_create_from_string(msg->add_conn.other.id ?
						msg->add_conn.other.id : msg->add_conn.other.address);
	if (other_id == NULL)
	{
		DBG1(DBG_CFG, "invalid ID: %s\n", msg->add_conn.other.id);
		my_id->destroy(my_id);
		goto destroy_hosts;
	}
	
	my_subnet = host_create_from_string(msg->add_conn.me.subnet ?
					msg->add_conn.me.subnet : msg->add_conn.me.address, IKE_PORT);
	if (my_subnet == NULL)
	{
		DBG1(DBG_CFG, "invalid subnet: %s\n", msg->add_conn.me.subnet);
		goto destroy_ids;
	}
	
	other_subnet = host_create_from_string(msg->add_conn.other.subnet ?
					msg->add_conn.other.subnet : msg->add_conn.other.address, IKE_PORT);
	if (other_subnet == NULL)
	{
		DBG1(DBG_CFG, "invalid subnet: %s\n", msg->add_conn.me.subnet);
		my_subnet->destroy(my_subnet);
		goto destroy_ids;
	}
				
	my_ts = traffic_selector_create_from_subnet(my_subnet,
				msg->add_conn.me.subnet ?  msg->add_conn.me.subnet_mask : 0,
				msg->add_conn.me.protocol, msg->add_conn.me.port);
	my_subnet->destroy(my_subnet);

	other_ts = traffic_selector_create_from_subnet(other_subnet, 
			msg->add_conn.other.subnet ?  msg->add_conn.other.subnet_mask : 0,
			msg->add_conn.other.protocol, msg->add_conn.other.port);
	other_subnet->destroy(other_subnet);

	if (msg->add_conn.me.ca)
	{
		if (streq(msg->add_conn.me.ca, "%same"))
		{
			my_ca_same = TRUE;
		}
		else
		{
			my_ca = identification_create_from_string(msg->add_conn.me.ca);
		}
	}
	if (msg->add_conn.other.ca)
	{
		if (streq(msg->add_conn.other.ca, "%same"))
		{
			other_ca_same = TRUE;
		}
		else
		{
			other_ca = identification_create_from_string(msg->add_conn.other.ca);
		}
	}
	if (msg->add_conn.me.cert)
	{
		x509_t *cert = load_end_certificate(msg->add_conn.me.cert, &my_id);

		if (my_ca == NULL && !my_ca_same && cert)
		{
			identification_t *issuer = cert->get_issuer(cert);

			my_ca = issuer->clone(issuer);
		}
	}
	if (msg->add_conn.other.cert)
	{
		x509_t *cert = load_end_certificate(msg->add_conn.other.cert, &other_id);

		if (other_ca == NULL && !other_ca_same && cert)
		{
			identification_t *issuer = cert->get_issuer(cert);

			other_ca = issuer->clone(issuer);
		}
	}
	if (other_ca_same && my_ca)
	{
		other_ca = my_ca->clone(my_ca);
	}
	else if (my_ca_same && other_ca)
	{
		my_ca = other_ca->clone(other_ca);
	}
	if (my_ca == NULL)
	{
		my_ca = identification_create_from_string("%any");
	}
	if (other_ca == NULL)
	{
		other_ca = identification_create_from_string("%any");
	}
	DBG2(DBG_CFG, "  my ca:   '%D'", my_ca);
	DBG2(DBG_CFG, "  other ca:'%D'", other_ca);
	DBG2(DBG_CFG, "  updown: '%s'", msg->add_conn.me.updown);

	connection = connection_create(msg->add_conn.name,
								   msg->add_conn.ikev2,
								   msg->add_conn.me.sendcert,
								   msg->add_conn.other.sendcert,
								   my_host, other_host,
								   msg->add_conn.dpd.delay,
								   msg->add_conn.rekey.reauth,
								   msg->add_conn.rekey.tries,
								   msg->add_conn.rekey.ike_lifetime,
								   msg->add_conn.rekey.ike_lifetime - msg->add_conn.rekey.margin,
								   msg->add_conn.rekey.margin * msg->add_conn.rekey.fuzz / 100);

	if (msg->add_conn.algorithms.ike)
	{
		char *proposal_string;
		char *strict = msg->add_conn.algorithms.ike + strlen(msg->add_conn.algorithms.ike) - 1;

		if (*strict == '!')
			*strict = '\0';
		else
			strict = NULL;

		while ((proposal_string = strsep(&msg->add_conn.algorithms.ike, ",")))
		{
			proposal = proposal_create_from_string(PROTO_IKE, proposal_string);
			if (proposal == NULL)
			{
				DBG1(DBG_CFG, "invalid IKE proposal string: %s", proposal_string);
				my_id->destroy(my_id);
				other_id->destroy(other_id);
				my_ts->destroy(my_ts);
				other_ts->destroy(other_ts);
				my_ca->destroy(my_ca);
				other_ca->destroy(other_ca);
				connection->destroy(connection);
				return;
			}
			connection->add_proposal(connection, proposal);
		}
		if (!strict)
		{
			proposal = proposal_create_default(PROTO_IKE);
			connection->add_proposal(connection, proposal);
		}
	}
	else
	{
		proposal = proposal_create_default(PROTO_IKE);
		connection->add_proposal(connection, proposal);
	}
	
	policy = policy_create(msg->add_conn.name, my_id, other_id,
						   msg->add_conn.auth_method,
						   msg->add_conn.rekey.ipsec_lifetime,
						   msg->add_conn.rekey.ipsec_lifetime - msg->add_conn.rekey.margin,
						   msg->add_conn.rekey.margin * msg->add_conn.rekey.fuzz / 100, 
						   msg->add_conn.me.updown, msg->add_conn.me.hostaccess,
						   msg->add_conn.mode, msg->add_conn.dpd.action);
	policy->add_my_traffic_selector(policy, my_ts);
	policy->add_other_traffic_selector(policy, other_ts);
	policy->add_authorities(policy, my_ca, other_ca);
	
	if (msg->add_conn.algorithms.esp)
	{
		char *proposal_string;
		char *strict = msg->add_conn.algorithms.esp + strlen(msg->add_conn.algorithms.esp) - 1;

		if (*strict == '!')
			*strict = '\0';
		else
			strict = NULL;
		
		while ((proposal_string = strsep(&msg->add_conn.algorithms.esp, ",")))
		{
			proposal = proposal_create_from_string(PROTO_ESP, proposal_string);
			if (proposal == NULL)
			{
				DBG1(DBG_CFG, "invalid ESP proposal string: %s", proposal_string);
				policy->destroy(policy);
				connection->destroy(connection);
				return;
			}
			policy->add_proposal(policy, proposal);
		}
		if (!strict)
		{
			proposal = proposal_create_default(PROTO_ESP);
			policy->add_proposal(policy, proposal);
		}
	}
	else
	{
		proposal = proposal_create_default(PROTO_ESP);
		policy->add_proposal(policy, proposal);
	}
	
	/* add to global connection list */
	charon->connections->add_connection(charon->connections, connection);
	DBG1(DBG_CFG, "added connection '%s': %H[%D]...%H[%D]",
		 msg->add_conn.name, my_host, my_id, other_host, other_id);
	/* add to global policy list */
	charon->policies->add_policy(charon->policies, policy);
	
	return;

	/* mopping up after parsing errors */

destroy_ids:
	my_id->destroy(my_id);
	other_id->destroy(other_id);

destroy_hosts:
	my_host->destroy(my_host);
	other_host->destroy(other_host);
}

/**
 * Delete a connection from the list
 */
static void stroke_del_conn(private_stroke_t *this, stroke_msg_t *msg)
{
	status_t status;
	
	pop_string(msg, &(msg->del_conn.name));
	DBG1(DBG_CFG, "received stroke: delete '%s'", msg->del_conn.name);
	
	status = charon->connections->delete_connection(charon->connections, 
													msg->del_conn.name);
	charon->policies->delete_policy(charon->policies, msg->del_conn.name);
	if (status == SUCCESS)
	{
		fprintf(this->out, "deleted connection '%s'\n", msg->del_conn.name);
	}
	else
	{
		fprintf(this->out, "no connection named '%s'\n", msg->del_conn.name);
	}
}

/**
 * initiate a connection by name
 */
static void stroke_initiate(private_stroke_t *this, stroke_msg_t *msg)
{
	initiate_job_t *job;
	connection_t *connection;
	policy_t *policy;
	ike_sa_t *init_ike_sa = NULL;
	signal_t signal;
	
	pop_string(msg, &(msg->initiate.name));
	DBG1(DBG_CFG, "received stroke: initiate '%s'", msg->initiate.name);
	
	connection = charon->connections->get_connection_by_name(charon->connections,
															 msg->initiate.name);
	if (connection == NULL)
	{
		if (msg->output_verbosity >= 0)
		{
			fprintf(this->out, "no connection named '%s'\n", msg->initiate.name);
		}
		return;
	}
	if (!connection->is_ikev2(connection))
	{
		connection->destroy(connection);
		return;
	}
	
	policy = charon->policies->get_policy_by_name(charon->policies, 
												  msg->initiate.name);
	if (policy == NULL)
	{
		if (msg->output_verbosity >= 0)
		{
			fprintf(this->out, "no policy named '%s'\n", msg->initiate.name);
		}
		connection->destroy(connection);
		return;
	}
	
	job = initiate_job_create(connection, NULL, policy);
	/*
	if (msg->output_verbosity < 0)
	{
	TODO: detach immediately if verbosity is SILENT. Local credential store
	is not threadsave yet, so this would cause crashes!!
		charon->job_queue->add(charon->job_queue, (job_t*)job);
		return;
}*/
	
	charon->bus->set_listen_state(charon->bus, TRUE);
	charon->job_queue->add(charon->job_queue, (job_t*)job);
	while (TRUE)
	{
		level_t level;
		int thread;
		ike_sa_t *ike_sa;
		char* format;
		va_list args;
		
		signal = charon->bus->listen(charon->bus, &level, &thread, &ike_sa, &format, &args);
		
		if ((init_ike_sa == NULL || ike_sa == init_ike_sa) &&
			level <= msg->output_verbosity)
		{
			if (vfprintf(this->out, format, args) < 0 ||
				fprintf(this->out, "\n") < 0 ||
				fflush(this->out))
			{
				charon->bus->set_listen_state(charon->bus, FALSE);
				break;
			}
		}
		
		switch (signal)
		{
			case CHILD_UP_SUCCESS:
			case CHILD_UP_FAILED:
			case IKE_UP_FAILED:
				if (ike_sa == init_ike_sa)
				{
					charon->bus->set_listen_state(charon->bus, FALSE);
					return;
				}
				continue;
			case CHILD_UP_START:
			case IKE_UP_START:
				if (init_ike_sa == NULL)
				{
					init_ike_sa = ike_sa;
				}
				continue;
			default:
				continue;
		}
	}
}

/**
 * route/unroute a policy (install SPD entries)
 */
static void stroke_route(private_stroke_t *this, stroke_msg_t *msg, bool route)
{
	route_job_t *job;
	connection_t *connection;
	policy_t *policy;
	
	pop_string(msg, &(msg->route.name));
	DBG1(DBG_CFG, "received stroke: %s '%s'",
		 route ? "route" : "unroute", msg->route.name);
	
	/* we wouldn't need a connection, but we only want to route policies
	 * whose connections are keyexchange=ikev2. */
	connection = charon->connections->get_connection_by_name(charon->connections,
															 msg->route.name);
	if (connection == NULL)
	{
		fprintf(this->out, "no connection named '%s'\n", msg->route.name);
		return;
	}
	if (!connection->is_ikev2(connection))
	{
		connection->destroy(connection);
		return;
	}
		
	policy = charon->policies->get_policy_by_name(charon->policies, 
												  msg->route.name);
	if (policy == NULL)
	{
		fprintf(this->out, "no policy named '%s'\n", msg->route.name);
		connection->destroy(connection);
		return;
	}
	fprintf(this->out, "%s policy '%s'\n",
			route ? "routing" : "unrouting", msg->route.name);
	job = route_job_create(connection, policy, route);
	charon->job_queue->add(charon->job_queue, (job_t*)job);
}

/**
 * terminate a connection by name
 */
static void stroke_terminate(private_stroke_t *this, stroke_msg_t *msg)
{
	pop_string(msg, &(msg->terminate.name));
	DBG1(DBG_CFG, "received stroke: terminate '%s'", msg->terminate.name);
	
	charon->ike_sa_manager->delete_by_name(charon->ike_sa_manager, msg->terminate.name);
}

/**
 * show status of daemon
 */
static void stroke_statusall(private_stroke_t *this, stroke_msg_t *msg)
{
	iterator_t *iterator;
	linked_list_t *list;
	host_t *host;
	connection_t *connection;
	policy_t *policy;
	ike_sa_t *ike_sa;
	char *name = NULL;

	leak_detective_status(this->out);
	
	fprintf(this->out, "Performance:\n");
	fprintf(this->out, "  worker threads: %d idle of %d,",
			charon->thread_pool->get_idle_threads(charon->thread_pool),
			charon->thread_pool->get_pool_size(charon->thread_pool));
	fprintf(this->out, " job queue load: %d,",
			charon->job_queue->get_count(charon->job_queue));
	fprintf(this->out, " scheduled events: %d\n",
			charon->event_queue->get_count(charon->event_queue));
	list = charon->socket->create_local_address_list(charon->socket);

	fprintf(this->out, "Listening on %d IP addresses:\n", list->get_count(list));
	while (list->remove_first(list, (void**)&host) == SUCCESS)
	{
		fprintf(this->out, "  %H\n", host);
		host->destroy(host);
	}
	list->destroy(list);
	
	if (msg->status.name)
	{
		pop_string(msg, &(msg->status.name));
		name = msg->status.name;
	}
	
	iterator = charon->connections->create_iterator(charon->connections);
	if (iterator->get_count(iterator) > 0)
	{
		fprintf(this->out, "Connections:\n");
	}
	while (iterator->iterate(iterator, (void**)&connection))
	{
		if (connection->is_ikev2(connection)
		&& (name == NULL || streq(name, connection->get_name(connection))))
		{
			fprintf(this->out, "%12s:  %H...%H\n",
					connection->get_name(connection),
					connection->get_my_host(connection),
					connection->get_other_host(connection));
		}
	}
	iterator->destroy(iterator);
	
	iterator = charon->policies->create_iterator(charon->policies);
	if (iterator->get_count(iterator) > 0)
	{
		fprintf(this->out, "Policies:\n");
	}
	while (iterator->iterate(iterator, (void**)&policy))
	{
		if (name == NULL || streq(name, policy->get_name(policy)))
		{
			fprintf(this->out, "%12s:  '%D'...'%D'\n",
					policy->get_name(policy),
					policy->get_my_id(policy),
					policy->get_other_id(policy));
		}
	}
	iterator->destroy(iterator);
	
	iterator = charon->ike_sa_manager->create_iterator(charon->ike_sa_manager);
	if (iterator->get_count(iterator) > 0)
	{
		fprintf(this->out, "Security Associations:\n");
	}
	while (iterator->iterate(iterator, (void**)&ike_sa))
	{
		bool ike_sa_printed = FALSE;
		child_sa_t *child_sa;
		iterator_t *children = ike_sa->create_child_sa_iterator(ike_sa);

		/* print IKE_SA */
		if (name == NULL || strncmp(name, ike_sa->get_name(ike_sa), strlen(name)) == 0)
		{
			fprintf(this->out, "%#K\n", ike_sa);
			ike_sa_printed = TRUE;
		}

		while (children->iterate(children, (void**)&child_sa))
		{
			bool child_sa_match = name == NULL ||
								  strncmp(name, child_sa->get_name(child_sa), strlen(name)) == 0;

			/* print IKE_SA if its name differs from the CHILD_SA's name */
			if (!ike_sa_printed && child_sa_match)
			{
				fprintf(this->out, "%#K\n", ike_sa);
				ike_sa_printed = TRUE;
			}

			/* print CHILD_SA */
			if (child_sa_match)
			{
				fprintf(this->out, "%#P\n", child_sa);
			}
		}
		children->destroy(children);
	}
	iterator->destroy(iterator);
}

/**
 * show status of daemon
 */
static void stroke_status(private_stroke_t *this, stroke_msg_t *msg)
{
	iterator_t *iterator;
	ike_sa_t *ike_sa;
	char *name = NULL;
	
	if (msg->status.name)
	{
		pop_string(msg, &(msg->status.name));
		name = msg->status.name;
	}
	
	iterator = charon->ike_sa_manager->create_iterator(charon->ike_sa_manager);
	while (iterator->iterate(iterator, (void**)&ike_sa))
	{
		bool ike_sa_printed = FALSE;
		child_sa_t *child_sa;
		iterator_t *children = ike_sa->create_child_sa_iterator(ike_sa);

		/* print IKE_SA */
		if (name == NULL || strncmp(name, ike_sa->get_name(ike_sa), strlen(name)) == 0)
		{
			fprintf(this->out, "%K\n", ike_sa);
			ike_sa_printed = TRUE;
		}

		while (children->iterate(children, (void**)&child_sa))
		{
			bool child_sa_match = name == NULL ||
								  strncmp(name, child_sa->get_name(child_sa), strlen(name)) == 0;

			/* print IKE_SA if its name differs from the CHILD_SA's name */
			if (!ike_sa_printed && child_sa_match)
			{
				fprintf(this->out, "%K\n", ike_sa);
				ike_sa_printed = TRUE;
			}

			/* print CHILD_SA */
			if (child_sa_match)
			{
				fprintf(this->out, "%P\n", child_sa);
			}
		}
		children->destroy(children);
	}
	iterator->destroy(iterator);
}

/**
 * list various information
 */
static void stroke_list(private_stroke_t *this, stroke_msg_t *msg)
{
	iterator_t *iterator;
	
	if (msg->list.flags & LIST_CERTS)
	{
		x509_t *cert;
		
		iterator = charon->credentials->create_cert_iterator(charon->credentials);
		if (iterator->get_count(iterator))
		{
			fprintf(this->out, "\n");
			fprintf(this->out, "List of X.509 End Entity Certificates:\n");
			fprintf(this->out, "\n");
		}
		while (iterator->iterate(iterator, (void**)&cert))
		{
			fprintf(this->out, "%#Q", cert, msg->list.utc);
			if (charon->credentials->has_rsa_private_key(
					charon->credentials, cert->get_public_key(cert)))
			{
				fprintf(this->out, ", has private key");
			}
			fprintf(this->out, "\n");
			
		}
		iterator->destroy(iterator);
	}
	if (msg->list.flags & LIST_CACERTS)
	{
		x509_t *cert;
		
		iterator = charon->credentials->create_cacert_iterator(charon->credentials);
		if (iterator->get_count(iterator))
		{
			fprintf(this->out, "\n");
			fprintf(this->out, "List of X.509 CA Certificates:\n");
			fprintf(this->out, "\n");
		}
		while (iterator->iterate(iterator, (void**)&cert))
		{
			fprintf(this->out, "%#Q\n", cert, msg->list.utc);
		}
		iterator->destroy(iterator);
	}
	if (msg->list.flags & LIST_CRLS)
	{
		crl_t *crl;
		
		iterator = charon->credentials->create_crl_iterator(charon->credentials);
		if (iterator->get_count(iterator))
		{
			fprintf(this->out, "\n");
			fprintf(this->out, "List of X.509 CRLs:\n");
			fprintf(this->out, "\n");
		}
		while (iterator->iterate(iterator, (void**)&crl))
		{
			fprintf(this->out, "%#U\n", crl, msg->list.utc);
		}
		iterator->destroy(iterator);
	}
}

/**
 * reread various information
 */
static void stroke_reread(private_stroke_t *this, stroke_msg_t *msg)
{
	if (msg->reread.flags & REREAD_CACERTS)
	{
		charon->credentials->load_ca_certificates(charon->credentials);
	}
	if (msg->reread.flags & REREAD_CRLS)
	{
		charon->credentials->load_crls(charon->credentials);
	}
}

signal_t get_signal_from_logtype(char *type)
{
	if      (strcasecmp(type, "any") == 0) return SIG_ANY;
	else if (strcasecmp(type, "mgr") == 0) return DBG_MGR;
	else if (strcasecmp(type, "ike") == 0) return DBG_IKE;
	else if (strcasecmp(type, "chd") == 0) return DBG_CHD;
	else if (strcasecmp(type, "job") == 0) return DBG_JOB;
	else if (strcasecmp(type, "cfg") == 0) return DBG_CFG;
	else if (strcasecmp(type, "knl") == 0) return DBG_KNL;
	else if (strcasecmp(type, "net") == 0) return DBG_NET;
	else if (strcasecmp(type, "enc") == 0) return DBG_ENC;
	else if (strcasecmp(type, "lib") == 0) return DBG_LIB;
	else return -1;
}

/**
 * set the verbosity debug output
 */
static void stroke_loglevel(private_stroke_t *this, stroke_msg_t *msg)
{
	signal_t signal;
	
	pop_string(msg, &(msg->loglevel.type));
	DBG1(DBG_CFG, "received stroke: loglevel %d for %s",
		 msg->loglevel.level, msg->loglevel.type);
	
	signal = get_signal_from_logtype(msg->loglevel.type);
	if (signal < 0)
	{
		fprintf(this->out, "invalid type (%s)!\n", msg->loglevel.type);
		return;
	}
	
	charon->outlog->set_level(charon->outlog, signal, msg->loglevel.level);
	charon->syslog->set_level(charon->syslog, signal, msg->loglevel.level);
}

/**
 * Implementation of private_stroke_t.stroke_receive.
 */
static void stroke_receive(private_stroke_t *this)
{
	stroke_msg_t *msg;
	u_int16_t msg_length;
	struct sockaddr_un strokeaddr;
	int strokeaddrlen = sizeof(strokeaddr);
	ssize_t bytes_read;
	int strokefd;
	int oldstate;
	
	/* ignore sigpipe. writing over the pipe back to the console
	 * only fails if SIGPIPE is ignored. */
	signal(SIGPIPE, SIG_IGN);
	
	/* disable cancellation by default */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	
	while (1)
	{
		/* wait for connections, but allow thread to terminate */
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate);
		strokefd = accept(this->socket, (struct sockaddr *)&strokeaddr, &strokeaddrlen);
		pthread_setcancelstate(oldstate, NULL);
		
		if (strokefd < 0)
		{
			DBG1(DBG_CFG, "accepting stroke connection failed: %m");
			continue;
		}
		
		/* peek the length */
		bytes_read = recv(strokefd, &msg_length, sizeof(msg_length), MSG_PEEK);
		if (bytes_read != sizeof(msg_length))
		{
			DBG1(DBG_CFG, "reading length of stroke message failed");
			close(strokefd);
			continue;
		}
		
		/* read message */
		msg = malloc(msg_length);
		bytes_read = recv(strokefd, msg, msg_length, 0);
		if (bytes_read != msg_length)
		{
			DBG1(DBG_CFG, "reading stroke message failed: %m");
			close(strokefd);
			continue;
		}
		
		this->out = fdopen(dup(strokefd), "w");
		if (this->out == NULL)
		{
			DBG1(DBG_CFG, "opening stroke output channel failed: %m");
			close(strokefd);
			free(msg);
			continue;
		}
		
		DBG3(DBG_CFG, "stroke message %b", (void*)msg, msg_length);
		
		switch (msg->type)
		{
			case STR_INITIATE:
				stroke_initiate(this, msg);
				break;
			case STR_ROUTE:
				stroke_route(this, msg, TRUE);
				break;
			case STR_UNROUTE:
				stroke_route(this, msg, FALSE);
				break;
			case STR_TERMINATE:
				stroke_terminate(this, msg);
				break;
			case STR_STATUS:
				stroke_status(this, msg);
				break;
			case STR_STATUS_ALL:
				stroke_statusall(this, msg);
				break;
			case STR_ADD_CONN:
				stroke_add_conn(this, msg);
				break;
			case STR_DEL_CONN:
				stroke_del_conn(this, msg);
				break;
			case STR_LOGLEVEL:
				stroke_loglevel(this, msg);
				break;
			case STR_LIST:
				stroke_list(this, msg);
				break;
			case STR_REREAD:
				stroke_reread(this, msg);
				break;
			default:
				DBG1(DBG_CFG, "received unknown stroke");
		}
		fclose(this->out);
		close(strokefd);
		free(msg);
	}
}

/**
 * Implementation of stroke_t.destroy.
 */
static void destroy(private_stroke_t *this)
{
	pthread_cancel(this->assigned_thread);
	pthread_join(this->assigned_thread, NULL);

	close(this->socket);
	unlink(socket_addr.sun_path);
	free(this);
}

/*
 * Described in header-file
 */
stroke_t *stroke_create()
{
	private_stroke_t *this = malloc_thing(private_stroke_t);
	mode_t old;

	/* public functions */
	this->public.destroy = (void (*)(stroke_t*))destroy;
	
	/* set up unix socket */
	this->socket = socket(AF_UNIX, SOCK_STREAM, 0);
	if (this->socket == -1)
	{
		DBG1(DBG_CFG, "could not create whack socket");
		free(this);
		return NULL;
	}
	
	old = umask(~S_IRWXU);
	if (bind(this->socket, (struct sockaddr *)&socket_addr, sizeof(socket_addr)) < 0)
	{
		DBG1(DBG_CFG, "could not bind stroke socket: %m");
		close(this->socket);
		free(this);
		return NULL;
	}
	umask(old);
	
	if (listen(this->socket, 0) < 0)
	{
		DBG1(DBG_CFG, "could not listen on stroke socket: %m");
		close(this->socket);
		unlink(socket_addr.sun_path);
		free(this);
		return NULL;
	}
	
	/* start a thread reading from the socket */
	if (pthread_create(&(this->assigned_thread), NULL, (void*(*)(void*))stroke_receive, this) != 0)
	{
		DBG1(DBG_CFG, "could not spawn stroke thread");
		close(this->socket);
		unlink(socket_addr.sun_path);
		free(this);
		return NULL;
	}
	
	return (&this->public);
}
