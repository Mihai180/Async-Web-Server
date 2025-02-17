// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <libaio.h>
#include <errno.h>
#include <limits.h>

#include "aws.h"
#include "utils/util.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/w_epoll.h"

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static io_context_t ctx;

// Funcție care actualizează evenimentele epoll
void update_epoll_events(struct connection *conn)
{
	// Verific starea conexiunii și setez evenimentele epoll corespunzătoare
	if (OUT_STATE(conn->state))
		w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
	else
		w_epoll_update_ptr_inout(epollfd, conn->sockfd, conn);
}

// Funcție care setează evenimentele epoll pe baza stării conexiunii
void set_epoll_events_based_on_state(struct connection *conn)
{
	// În funcție de starea conexiunii, actualizez epoll pentru intrare, ieșire sau ambele
	if (OUT_STATE(conn->state))
		w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
	else if (conn->state == STATE_RECEIVING_DATA || conn->state == STATE_REQUEST_RECEIVED)
		w_epoll_update_ptr_in(epollfd, conn->sockfd, conn);
	else
		w_epoll_update_ptr_inout(epollfd, conn->sockfd, conn);
}

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = (struct connection *)p->data;

	memcpy(conn->request_path, buf, len);
	conn->request_path[len] = '\0';
	conn->have_path = 1;

	return 0;
}

static void connection_prepare_send_reply_header(struct connection *conn)
{
	// Tipul MIME pentru răspuns
	const char *mime_type = "application/octet-stream";

	// Creez antetul HTTP și îl stochez în buffer-ul conexiunii
	int len = snprintf(conn->send_buffer, sizeof(conn->send_buffer),
		"HTTP/1.1 200 OK\r\n"
		"Connection: close\r\n"
		"Content-Length: %zu\r\n"
		"Content-Type: %s\r\n"
		"\r\n",
		conn->file_size,
		mime_type
	);

	// Verific dacă lungimea antetului depășește dimensiunea buffer-ului
	if (len < 0 || (size_t)len >= sizeof(conn->send_buffer)) {
		conn->state = STATE_CONNECTION_CLOSED; // Închide conexiunea în caz de eroare
		return;
	}

	// Setez lungimea datelor de trimis și poziția curentă
	conn->send_len = (size_t)len;
	conn->send_pos = 0;
}

static void connection_prepare_send_404(struct connection *conn)
{
	// Mesaj pentru răspunsul HTTP 404
	const char *msg_404 =
		"HTTP/1.1 404 Not Found\r\n"
		"Connection: close\r\n"
		"Content-Length: 13\r\n"
		"Content-Type: text/plain\r\n"
		"\r\n"
		"404 Not Found";

	// Determin lungimea mesajului și îl copiez în buffer-ul de trimitere
	size_t len = strlen(msg_404);

	memcpy(conn->send_buffer, msg_404, len);

	// Setez lungimea și poziția curentă pentru trimiterea mesajului
	conn->send_len = len;
	conn->send_pos = 0;
}

static enum resource_type connection_get_resource_type(struct connection *conn)
{
	// Verific dacă calea începe cu "/static/" sau "/dynamic/"
	if (strncmp(conn->request_path, "/static/", 8) == 0)
		return RESOURCE_TYPE_STATIC; // Resursă statică
	else if (strncmp(conn->request_path, "/dynamic/", 9) == 0)
		return RESOURCE_TYPE_DYNAMIC; // Resursă dinamică
	else
		return RESOURCE_TYPE_NONE; // Tip necunoscut
}


struct connection *connection_create(int sockfd)
{
	// Aloc memorie pentru structura conexiunii
	struct connection *conn = calloc(1, sizeof(*conn));

	// Verific alocarea memoriei
	if (!conn)
		return NULL;


	// Inițializez câmpurile structurii
	conn->sockfd = sockfd; // Setez descriptorul de fișier al socket-ului
	conn->fd = -1; // Fișierul nu este deschis
	conn->eventfd = -1; // Evenimentul nu este inițializat
	conn->file_size = 0; // Dimensiunea fișierului este inițial zero
	conn->recv_len = 0; // Lungimea datelor recepționate este zero
	conn->send_len = 0; // Lungimea datelor de trimis este zero
	conn->send_pos = 0; // Poziția curentă în buffer-ul de trimitere
	conn->file_pos = 0; // Poziția curentă în fișier
	conn->async_read_len = 0; // Lungimea citirii asincrone este zero
	conn->have_path = 0; // Nu există cale încă
	conn->res_type = RESOURCE_TYPE_NONE; // Tipul resursei este necunoscut
	conn->state = STATE_INITIAL; // Starea inițială a conexiunii

	// Inițializez parser-ul HTTP
	http_parser_init(&conn->request_parser, HTTP_REQUEST);
	conn->request_parser.data = conn;

	return conn; // Returnez conexiunea creată
}

void connection_start_async_io(struct connection *conn)
{
	// Pregătesc blocul de control pentru I/O asincron
	memset(&conn->iocb, 0, sizeof(conn->iocb));
	conn->piocb[0] = &conn->iocb;

	// Calculez cât mai este de citit din fișier
	size_t remaining = conn->file_size - conn->file_pos;

	if (remaining > sizeof(conn->send_buffer))
		remaining = sizeof(conn->send_buffer);

	// Pregătesc operațiunea de citire folosind io_prep_pread
	io_prep_pread(&conn->iocb, conn->fd, conn->send_buffer, remaining, conn->file_pos);
	conn->iocb.data = conn;

	// Trimit cererea de I/O asincron
	int ret = io_submit(ctx, 1, conn->piocb);

	if (ret < 0) // Dacă operațiunea eșuează
		conn->state = STATE_CONNECTION_CLOSED; // Închid conexiunea
	else
		conn->state = STATE_ASYNC_ONGOING; // Starea devine operațiune în desfășurare
}

void connection_remove(struct connection *conn)
{
	if (!conn) // Dacă conexiunea este NULL, nu fac nimic
		return;

	// Elimin conexiunea din epoll
	w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	// Închid descriptorii de fișiere asociați
	close(conn->sockfd);
	if (conn->fd >= 0)
		close(conn->fd);

	if (conn->eventfd >= 0)
		close(conn->eventfd);

	// Eliberez memoria alocată conexiunii
	free(conn);
}

void handle_new_connection(void)
{
	while (1) {
		// Accept o nouă conexiune
		int cfd = accept(listenfd, NULL, NULL);

		if (cfd < 0) { // Dacă accept eșuează
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break; // Nu mai sunt conexiuni de procesat

			break; // Alte erori
		}

		// Setez socket-ul conexiunii ca non-blocant
		int flags = fcntl(cfd, F_GETFL, 0);

		fcntl(cfd, F_SETFL, flags | O_NONBLOCK);

		// Creez un handler pentru noua conexiune
		struct connection *conn = connection_create(cfd);

		if (!conn) { // Dacă nu poate fi creat
			close(cfd); // Închide conexiunea
			continue;
		}

		// Adaug conexiunea la epoll pentru evenimente de intrare
		conn->state = STATE_RECEIVING_DATA;
		w_epoll_add_ptr_in(epollfd, cfd, conn);
	}
}

void receive_data(struct connection *conn)
{
	// Citesc date din socket și le stochează în buffer-ul de recepție
	ssize_t rc = recv(conn->sockfd,
					  conn->recv_buffer + conn->recv_len,
					  sizeof(conn->recv_buffer) - conn->recv_len,
					  0);
	if (rc < 0) { // Dacă recepția eșuează
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return; // Nu sunt date disponibile momentan

		conn->state = STATE_CONNECTION_CLOSED; // Închid conexiunea în caz de eroare
		return;
	}
	if (rc == 0) { // Dacă conexiunea este închisă de client
		conn->state = STATE_CONNECTION_CLOSED;
		return;
	}
	// Actualizez lungimea datelor recepționate și setez starea conexiunii
	conn->recv_len = conn->recv_len + rc;
	conn->state = STATE_REQUEST_RECEIVED;
}

int connection_open_file(struct connection *conn)
{
	// Verific tipul resursei cerute de client
	if (conn->res_type == RESOURCE_TYPE_STATIC) {
		const char *subpath = conn->request_path + 8; // Elimin prefixul "/static/"
													// pentru a obține subcalea fișierului

		// Determin lungimea subcăii fișierului
		size_t subpath_len = strlen(subpath);

		// Verific dacă subcalea se termină cu un punct
		if (subpath_len > 0 && subpath[subpath_len - 1] == '.') {
			// Creez un buffer pentru subcalea igienizată (fără punct final)
			char sanitized_subpath[BUFSIZ];

			// Copiez subcalea fără ultimul caracter (punctul final)
			memcpy(sanitized_subpath, subpath, subpath_len - 1);
			sanitized_subpath[subpath_len - 1] = '\0';
			// Aloc memorie pentru copia subcăii și verific alocarea
			subpath = strdup(sanitized_subpath);
			if (!subpath)
				return -1; // Dacă alocarea eșuează, returnez eroare
		}

		// Dacă subcalea este goală, eliberez memoria și returnez eroare
		if (strlen(subpath) == 0) {
			free((void *)subpath);
			return -1;
		}

		// Creez un buffer pentru subcalea finalizată
		char finalized_subpath[BUFSIZ];

		// Dacă subcalea nu are extensia ".dat", o adaug
		if (strstr(subpath, ".dat") == NULL) {
			if (snprintf(finalized_subpath, sizeof(finalized_subpath), "%s.dat", subpath) >= sizeof(finalized_subpath))
				return -1;

			subpath = finalized_subpath;
		}

		// Verific dacă subcalea conține ..
		if (strstr(subpath, ".."))
			return -1;


		// Construiesc calea absolută către fișierul static
		if (snprintf(conn->filename, sizeof(conn->filename),
		"%s%s", AWS_ABS_STATIC_FOLDER, subpath) >= sizeof(conn->filename)) {
			return -1;
		}

	} else if (conn->res_type == RESOURCE_TYPE_DYNAMIC) {
		// Elimin prefixul "/dynamic/" pentru fișiere dinamice
		const char *subpath = conn->request_path + 9;

		// Determin lungimea subcăii
		size_t subpath_len = strlen(subpath);

		// Verific dacă subcalea se termină cu un punct
		if (subpath_len > 0 && subpath[subpath_len - 1] == '.') {
			char sanitized_subpath[BUFSIZ];

			// Copiez subcalea fără ultimul caracter (punctul final)
			memcpy(sanitized_subpath, subpath, subpath_len - 1);
			sanitized_subpath[subpath_len - 1] = '\0';

			// Creez o copie a subcăii și verific alocarea memoriei
			subpath = strdup(sanitized_subpath);
			if (!subpath)
				return -1;
		}

	// Dacă subcalea este goală, eliberez memoria și returnez eroare
	if (strlen(subpath) == 0) {
		free((void *)subpath);
		return -1;
	}

	// Creez buffer pentru subcalea finalizată
	char finalized_subpath[BUFSIZ];

	// Adaug extensia ".dat" dacă lipsește
	if (strstr(subpath, ".dat") == NULL) {
		if (snprintf(finalized_subpath, sizeof(finalized_subpath), "%s.dat", subpath) >= sizeof(finalized_subpath))
			return -1;

		subpath = finalized_subpath;
	}

	// Verific dacă subcalea conține ..
	if (strstr(subpath, ".."))
		return -1;


	// Construiesc calea absolută către fișierul dinamic
	if (snprintf(conn->filename, sizeof(conn->filename), "%s%s",
	AWS_ABS_DYNAMIC_FOLDER, subpath) >= sizeof(conn->filename)) {
		return -1;
	}

	} else {
		return -1; // Tip de resursă necunoscut
	}

	// Deschid fișierul în mod doar citire
	conn->fd = open(conn->filename, O_RDONLY);
	if (conn->fd < 0) { // Dacă deschiderea eșuează
		connection_prepare_send_404(conn); // Pregătesc un răspuns 404
		conn->state = STATE_SENDING_404;
		w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
		return -1;
	}

	// Obțin informații despre fișier
	struct stat st;

	if (fstat(conn->fd, &st) < 0) { // Dacă stat eșuează
		close(conn->fd); // Închid fișierul
		conn->fd = -1;
		return -1;
	}

	// Actualizez dimensiunea fișierului și poziția curentă
	conn->file_size = (size_t)st.st_size;
	conn->file_pos  = 0;

	return 0; // Succes
}

void connection_complete_async_io(struct connection *conn)
{
	// Declar un array pentru evenimente de I/O asincron
	struct io_event events[1];

	// Inițializez array-ul de evenimente cu zero pentru a preveni valori nedeterminate
	memset(events, 0, sizeof(events));

	// Aștept finalizarea operației asincrone de I/O
	// Parametrii: contextul, numărul minim/maxim de evenimente,
	// buffer-ul de evenimente, timeout-ul (NULL - infinit)
	int nr = io_getevents(ctx, 1, 1, events, NULL);

	// Verific dacă există evenimente finalizate
	if (nr < 1) // Dacă nu a fost procesat niciun eveniment, ies din funcție
		return;

	// Verific rezultatul operației de I/O asincron
	if (events[0].res < 0) { // Dacă operația a eșuat (rezultatul este negativ)
		conn->state = STATE_CONNECTION_CLOSED;
		return;
	}

	// Obțin numărul de octeți procesați cu succes din operația de I/O
	ssize_t bytes = events[0].res;

	// Actualizeaz lungimea datelor citite asincron în structura conexiunii
	conn->async_read_len = (size_t)bytes;
	// Actualizeaz poziția curentă în fișier, adăugând numărul de octeți citiți
	conn->file_pos = conn->file_pos + (size_t)bytes;

	// Pregătesc datele pentru trimitere prin socket
	// Setez lungimea datelor ce urmează să fie trimise și resetez poziția de trimitere
	conn->send_len = conn->async_read_len;
	conn->send_pos = 0;
	// Verific dacă mai există date de citit sau de trimis
	if (bytes == 0)
		conn->state = STATE_DATA_SENT;
	else
		conn->state = STATE_SENDING_DATA;
}

int parse_header(struct connection *conn)
{
	http_parser_settings settings_on_path = {
		.on_message_begin = 0,
		.on_header_field = 0,
		.on_header_value = 0,
		.on_path = aws_on_path_cb,
		.on_url = 0,
		.on_fragment = 0,
		.on_query_string = 0,
		.on_body = 0,
		.on_headers_complete = 0,
		.on_message_complete = 0
	};

	// Execut parser-ul HTTP pentru buffer-ul recepționat
	// Parametrii: parser-ul asociat conexiunii, setările, buffer-ul de date și lungimea acestuia
	size_t nparsed = http_parser_execute(&conn->request_parser,
										 &settings_on_path,
										 conn->recv_buffer,
										 conn->recv_len);

	// Verific dacă parser-ul a procesat toate datele sau dacă calea cerută nu a fost extrasă
	if (nparsed < conn->recv_len || !conn->have_path)
		return -1;

	// Dacă parsarea a reușit, returnez succes
	return 0;
}

enum connection_state connection_send_static(struct connection *conn)
{
	// Verific dacă descriptorul fișierului este valid
	if (conn->fd < 0)
		return STATE_CONNECTION_CLOSED;


	// Inițializez offset-ul din fișier cu poziția curentă
	off_t offset = conn->file_pos;
	// Calculez dimensiunea rămasă de trimis
	size_t remaining = conn->file_size - conn->file_pos;

	// Dacă nu mai sunt date de trimis
	if (remaining == 0) {
		shutdown(conn->sockfd, SHUT_WR); // Închid scrierea pe socket
		conn->state = STATE_DATA_SENT; // Actualizez starea conexiunii
		return STATE_DATA_SENT; // Returnez succes
	}

	// Setez dimensiunea maximă a unui chunk de date de trimis
	size_t chunk_size;

	if (remaining > SSIZE_MAX)
		chunk_size = SSIZE_MAX;
	else
		chunk_size = remaining;
	// Trimit datele direct din fișier către socket folosind sendfile
	ssize_t sent = sendfile(conn->sockfd, conn->fd, &offset, chunk_size);

	// Dacă trimiterea datelor a eșuat
	if (sent < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			// Dacă socket-ul nu este pregătit pentru trimitere
			set_epoll_events_based_on_state(conn); // Actualizez evenimentele epoll
			return STATE_SENDING_DATA; // Continui să trimit date ulterior
		}
		// În caz de alte erori, închid conexiunea
		conn->state = STATE_CONNECTION_CLOSED;
		return STATE_CONNECTION_CLOSED;
	}

	// Actualizez poziția curentă în fișier după trimiterea datelor
	conn->file_pos = offset;

	// Verific dacă toate datele din fișier au fost trimise
	if (conn->file_pos >= conn->file_size) {
		shutdown(conn->sockfd, SHUT_WR); // Închid scrierea pe socket
		conn->state = STATE_DATA_SENT; // Actualizez starea conexiunii
		return STATE_DATA_SENT; // Returnez succes
	}

	// Dacă mai sunt date de trimis, actualizez evenimentele epoll
	set_epoll_events_based_on_state(conn);
	conn->state = STATE_SENDING_DATA;
	return STATE_SENDING_DATA;
}

int connection_send_data(struct connection *conn)
{
	// Calculez numărul de octeți rămași de trimis
	size_t to_send = conn->send_len - conn->send_pos;

	// Dacă nu mai sunt date de trimis, returnez 0
	if (to_send == 0)
		return 0;

	// Trimit datele din buffer-ul de trimitere al conexiunii prin socket
	// Parametrii: socket-ul (conn->sockfd), adresa datelor (conn->send_buffer + conn->send_pos),
	// dimensiunea datelor (to_send), fără flag-uri (0)
	ssize_t rc = send(conn->sockfd,
					  conn->send_buffer + conn->send_pos,
					  to_send,
					  0);

	// Verific dacă trimiterea datelor a eșuat
	if (rc < 0) {
		// Dacă socket-ul nu este pregătit pentru scriere
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0; // Indică faptul că trimiterea trebuie reîncercată

		// În caz de alte erori, returnez -1 pentru a semnala o eroare
		return -1;
	}
	// Actualizez poziția curentă în buffer-ul de trimitere
	conn->send_pos = conn->send_pos + (size_t)rc;
	// Returnez numărul de octeți trimiși
	return (int)rc;
}


int connection_send_dynamic(struct connection *conn)
{
	// Trimit date din buffer-ul conexiunii folosind funcția connection_send_data
	int rc = connection_send_data(conn);

	// Dacă trimiterea datelor a eșuat, returnez -1 pentru a semnala eroarea
	if (rc < 0)
		return -1;

	// Dacă mai sunt date de trimis din buffer, returnez 0 pentru a reîncerca ulterior
	if (conn->send_pos < conn->send_len)
		return 0;

	// Dacă mai sunt date în fișier, încep o nouă operație asincronă de citire
	if (conn->file_pos < conn->file_size)
		connection_start_async_io(conn); // Inițiază citirea asincronă
	else
		conn->state = STATE_DATA_SENT; // Altfel, toate datele au fost trimise
	// Returnez 0 pentru a indica succesul
	return 0;
}


void handle_input(struct connection *conn)
{
	// Verific starea curentă a conexiunii și acționez corespunzător
	switch (conn->state) {
	case STATE_RECEIVING_DATA:
		// Dacă conexiunea este în starea de recepționare a datelor,
		// apelez funcția pentru a citi datele din socket
		receive_data(conn);
		break;

	case STATE_REQUEST_RECEIVED:
		// Dacă s-a primit o cerere completă, parsez antetul cererii HTTP
		if (parse_header(conn) < 0) {
			// Dacă parsarea a eșuat, pregătesc un răspuns HTTP 404
			connection_prepare_send_404(conn);
			conn->state = STATE_SENDING_404; // Setez starea la trimiterea răspunsului 404

			// Actualizez evenimentele epoll pentru a permite scrierea pe socket
			w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
			break;
		}
		// Determin tipul resursei cerute pe baza căii extrase din cerere
		conn->res_type = connection_get_resource_type(conn);

		// Dacă tipul resursei este necunoscut sau deschiderea fișierului a eșuat
		if (conn->res_type == RESOURCE_TYPE_NONE ||
			connection_open_file(conn) < 0) {
				// Pregătesc un răspuns HTTP 404 și actualizez starea
			connection_prepare_send_404(conn);
			conn->state = STATE_SENDING_404;
			set_epoll_events_based_on_state(conn); // Actualizez evenimentele epoll
		} else {
			// Dacă resursa a fost identificată și fișierul a fost deschis cu succes
			// Pregătesc antetul răspunsului HTTP
			connection_prepare_send_reply_header(conn);
			conn->state = STATE_SENDING_HEADER; // Setez starea pentru trimiterea antetului
			set_epoll_events_based_on_state(conn); // Actualizez evenimentele epoll
		}
		break;

	case STATE_ASYNC_ONGOING:
		// Dacă operația asincronă este în desfășurare, finalizez operația
		connection_complete_async_io(conn);
		break;

	default:
		// Pentru orice altă stare neprevăzută, afișez un mesaj de eroare și închid conexiunea
		printf("shouldn't get here %d\n", conn->state);
		conn->state = STATE_CONNECTION_CLOSED;
		break;
	}
}

void handle_output(struct connection *conn)
{
	// Verific starea curentă a conexiunii pentru a decide cum să gestionez ieșirea
	switch (conn->state) {
	case STATE_SENDING_HEADER: {
		// În această stare, serverul trimite antetul HTTP
		int rc = connection_send_data(conn); // Trimite datele din buffer-ul de trimitere
			if (rc < 0) {
				// Dacă trimiterea a eșuat, închid conexiunea
				conn->state = STATE_CONNECTION_CLOSED;
			} else if ((size_t)rc == 0 && conn->send_pos < conn->send_len) {
				// Dacă mai sunt date de trimis și socket-ul nu este pregătit, actualizez evenimentele epoll
				set_epoll_events_based_on_state(conn);
			} else if (conn->send_pos == conn->send_len) {
				// Dacă toate datele din antet au fost trimise
				conn->state = STATE_HEADER_SENT;
				if (conn->res_type == RESOURCE_TYPE_STATIC)
					conn->state = STATE_SENDING_DATA; // Dacă este resursă statică, trec la trimiterea datelor
				else
					connection_start_async_io(conn); // Dacă este resursă dinamică, inițiez citirea asincronă

				// Actualizez evenimentele epoll pentru a permite trimiterea ulterioară
				set_epoll_events_based_on_state(conn);
			}
			break;
		}
	case STATE_SENDING_404: {
		// În această stare, serverul trimite un răspuns HTTP 404
		int rc = connection_send_data(conn); // Trimite datele 404 din buffer-ul de trimitere
			if (rc < 0) {
				// Dacă trimiterea a eșuat, închid conexiunea
				conn->state = STATE_CONNECTION_CLOSED;
			} else if ((size_t)rc == 0 && conn->send_pos < conn->send_len) {
				// Dacă mai sunt date de trimis și socket-ul nu este pregătit, actualizez evenimentele epoll
				set_epoll_events_based_on_state(conn);
			} else if (conn->send_pos == conn->send_len) {
				// Dacă toate datele 404 au fost trimise
				conn->state = STATE_404_SENT;
				set_epoll_events_based_on_state(conn);
			}
			break;
	}
	case STATE_SENDING_DATA: {
		// În această stare, serverul trimite conținutul resursei solicitate
		if (conn->res_type == RESOURCE_TYPE_STATIC) {
			// Dacă resursa este statică, trimit datele din fișier folosind sendfile
			enum connection_state st = connection_send_static(conn);

			conn->state = st; // Actualizez starea conexiunii pe baza rezultatului
		} else {
			// Dacă resursa este dinamică, trimit datele procesate dinamic
			if (connection_send_dynamic(conn) < 0)
				conn->state = STATE_CONNECTION_CLOSED; // Închid conexiunea în caz de eroare
		}
		// Actualizez evenimentele epoll pentru a continua trimiterea datelor
		set_epoll_events_based_on_state(conn);
		break;
	}
	default:
		ERR("Unexpected state\n");
		exit(1);
	}
}

void handle_client(uint32_t event, struct connection *conn)
{
	// Verific dacă conexiunea este validă și nu a fost închisă
	if (!conn || conn->state == STATE_CONNECTION_CLOSED)
		return;

	// Verific dacă evenimentul primit indică disponibilitate pentru citire (EPOLLIN)
	if (event & EPOLLIN)
		handle_input(conn); // Procesez datele de intrare pentru această conexiune

	// După procesarea intrării, verific din nou starea conexiunii
	if (conn->state == STATE_CONNECTION_CLOSED) {
		connection_remove(conn); // Dacă conexiunea este închisă, eliberez resursele
		return;
	}

	// Verific dacă evenimentul primit indică disponibilitate pentru scriere (EPOLLOUT)
	if (event & EPOLLOUT)
		handle_output(conn); // Procesez datele de ieșire pentru această conexiune

	// După procesarea ieșirii, verific dacă conexiunea trebuie închisă
	if (conn->state == STATE_CONNECTION_CLOSED ||
		conn->state == STATE_404_SENT ||
		conn->state == STATE_DATA_SENT) {
		connection_remove(conn); // Eliberez resursele conexiunii
	}
}

int main(void)
{
	int rc;

	// Ignor semnalul SIGPIPE pentru a evita terminarea procesului când un socket este închis de client
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
		exit(EXIT_FAILURE); // Dacă configurarea semnalului eșuează, termin procesul

	// Inițializez contextul I/O asincron
	memset(&ctx, 0, sizeof(ctx)); // Setez toate câmpurile contextului la zero
	rc = io_setup(128, &ctx); // Configurez contextul pentru până la 128 de operații I/O simultane
	DIE(rc < 0, "io_setup"); // Termin procesul dacă inițializarea I/O asincron eșuează

	// Creez un descriptor epoll pentru multiplexarea evenimentelor
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "epoll_create"); // Termin procesul dacă epoll nu poate fi creat

	// Creez un socket pentru ascultarea conexiunilor client
	listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener"); // Termin procesul dacă socket-ul nu poate fi creat

	// Obțin flag-urile curente ale socket-ului
	int flags = fcntl(listenfd, F_GETFL, 0);

	// Configurez socket-ul pentru a funcționa în mod non-blocant
	fcntl(listenfd, F_SETFL, flags | O_NONBLOCK);

	// Adaug socket-ul de ascultare în epoll pentru evenimente de intrare (EPOLLIN)
	w_epoll_add_fd_in(epollfd, listenfd);

	/* Uncomment the following line for debugging. */
	// dlog(LOG_INFO, "Server waiting for connections on port %d\n", AWS_LISTEN_PORT);

	/* server main loop */
	while (1) { // Bucla principală a serverului
		struct epoll_event rev; // Structură pentru stocarea unui eveniment epoll

		// Aștept un eveniment pe descriptorii monitorizați
		int n = epoll_wait(epollfd, &rev, 1, -1);

		if (n < 0) { // Verific dacă a apărut o eroare
			if (errno == EINTR) // Dacă apelul a fost întrerupt de un semnal
				continue; // Reiau așteptarea evenimentelor

			break;
		}

		if (n == 0) // Dacă nu sunt evenimente, continui bucla
			continue;

		if (rev.data.fd == listenfd) {
			// Dacă evenimentul este pe socket-ul de ascultare, procesez o nouă conexiune
			handle_new_connection();
		} else {
			// Dacă evenimentul este pe un socket de client, procesez conexiunea
			struct connection *conn = (struct connection *)rev.data.ptr;

			handle_client(rev.events, conn);
		}
	}

	// Curăț resursele înainte de a închide serverul
	close(listenfd); // Închid socket-ul de ascultare
	close(epollfd); // Închid descriptorul epoll
	io_destroy(ctx); // Distrug contextul I/O asincron
	return 0; // Închei programul cu succes
}
