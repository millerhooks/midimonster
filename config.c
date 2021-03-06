#include <string.h>
#include <ctype.h>
#include "midimonster.h"
#include "config.h"
#include "backend.h"

static enum {
	none,
	backend_cfg,
	instance_cfg,
	map
} parser_state = none;

typedef enum {
	map_ltr,
	map_rtl,
	map_bidir
} map_type;

static backend* current_backend = NULL;
static instance* current_instance = NULL;

static char* config_trim_line(char* in){
	ssize_t u;
	//trim front
	for(; *in && !isgraph(*in); in++){
	}

	//trim back
	for(u = strlen(in); u >= 0 && !isgraph(in[u]); u--){
		in[u] = 0;
	}

	return in;
}

static int config_map(char* to_raw, char* from_raw){
	//create a copy because the original pointer may be used multiple times
	char* to = strdup(to_raw), *from = strdup(from_raw);
	char* chanspec_to = to, *chanspec_from = from;
	instance* instance_to = NULL, *instance_from = NULL;
	channel* channel_from = NULL, *channel_to = NULL;
	int rv = 1;

	if(!from || !to){
		fprintf(stderr, "Failed to allocate memory\n");
		return 1;
	}

	//separate channel spec from instance
	for(; *chanspec_to && *chanspec_to != '.'; chanspec_to++){
	}

	for(; *chanspec_from && *chanspec_from != '.'; chanspec_from++){
	}

	if(!*chanspec_to || !*chanspec_from){
		fprintf(stderr, "Mapping does not contain a proper instance specification\n");
		goto done;
	}

	//terminate
	*chanspec_to = *chanspec_from = 0;
	chanspec_to++;
	chanspec_from++;

	//find matching instances
	instance_to = instance_match(to);
	instance_from = instance_match(from);

	if(!instance_to || !instance_from){
		fprintf(stderr, "No such instance %s\n", instance_from ? to : from);
		goto done;
	}

	channel_from = instance_from->backend->channel(instance_from, chanspec_from);
	channel_to = instance_to->backend->channel(instance_to, chanspec_to);

	if(!channel_from || !channel_to){
		fprintf(stderr, "Failed to parse channel specifications\n");
		goto done;
	}

	rv = mm_map_channel(channel_from, channel_to);
done:
	free(from);
	free(to);
	return rv;
}

int config_read(char* cfg_file){
	int rv = 1;
	size_t line_alloc = 0;
	ssize_t status;
	map_type mapping_type = map_rtl;
	char* line_raw = NULL, *line, *separator;
	FILE* source = fopen(cfg_file, "r");
	if(!source){
		fprintf(stderr, "Failed to open configuration file for reading\n");
		return 1;
	}

	for(status = getline(&line_raw, &line_alloc, source); status >= 0; status = getline(&line_raw, &line_alloc, source)){
		line = config_trim_line(line_raw);
		if(*line == ';' || strlen(line) == 0){
			//skip comments
			continue;
		}
		if(*line == '[' && line[strlen(line) - 1] == ']'){
			if(!strncmp(line, "[backend ", 9)){
				//backend configuration
				parser_state = backend_cfg;
				line[strlen(line) - 1] = 0;
				current_backend = backend_match(line + 9);

				if(!current_backend){
					fprintf(stderr, "Cannot configure unknown backend %s\n", line + 9);
					goto bail;
				}
			}
			else if(!strcmp(line, "[map]")){
				//mapping configuration
				parser_state = map;
			}
			else{
				//backend instance configuration
				parser_state = instance_cfg;
				
				//trim braces
				line[strlen(line) - 1] = 0;
				line++;

				//find separating space and terminate
				for(separator = line; *separator && *separator != ' '; separator++){
				}
				if(!*separator){
					fprintf(stderr, "No instance name specified for backend %s\n", line);
					goto bail;
				}
				*separator = 0;
				separator++;

				current_backend = backend_match(line);
				if(!current_backend){
					fprintf(stderr, "No such backend %s\n", line);
					goto bail;
				}

				if(instance_match(separator)){
					fprintf(stderr, "Duplicate instance name %s\n", separator);
					goto bail;
				}

				//validate instance name
				if(strchr(separator, ' ') || strchr(separator, '.')){
					fprintf(stderr, "Invalid instance name %s\n", separator);
					goto bail;
				}

				current_instance = current_backend->create();
				if(!current_instance){
					fprintf(stderr, "Failed to instantiate backend %s\n", line);
					goto bail;
				}

				current_instance->name = strdup(separator);
				current_instance->backend = current_backend;
				fprintf(stderr, "Created %s instance %s\n", line, separator);
			}
		}
		else if(parser_state == map){
			mapping_type = map_rtl;
			//find separator
			for(separator = line; *separator && *separator != '<' && *separator != '>'; separator++){
			}

			switch(*separator){
				case '>':
					mapping_type = map_ltr;
					//fall through
				case '<': //default
					*separator = 0;
					separator++;
					break;
				case 0:
				default:
					fprintf(stderr, "Not a channel mapping: %s\n", line);
					goto bail;
			}

			if((mapping_type == map_ltr && *separator == '<')
					|| (mapping_type == map_rtl && *separator == '>')){
				mapping_type = map_bidir;
				separator++;
			}

			line = config_trim_line(line);
			separator = config_trim_line(separator);

			if(mapping_type == map_ltr || mapping_type == map_bidir){
				if(config_map(separator, line)){
					fprintf(stderr, "Failed to map channel %s to %s\n", line, separator);
					goto bail;
				}
			}
			if(mapping_type == map_rtl || mapping_type == map_bidir){
				if(config_map(line, separator)){
					fprintf(stderr, "Failed to map channel %s to %s\n", separator, line);
					goto bail;
				}
			}
		}
		else{
			//pass to parser
			//find separator
			separator = strchr(line, '=');
			if(!separator){
				fprintf(stderr, "Not an assignment: %s\n", line);
				goto bail;
			}

			*separator = 0;
			separator++;
			line = config_trim_line(line);
			separator = config_trim_line(separator);

			if(parser_state == backend_cfg && current_backend->conf(line, separator)){
				fprintf(stderr, "Failed to configure backend %s\n", current_backend->name);
				goto bail;
			}
			else if(parser_state == instance_cfg && current_backend->conf_instance(current_instance, line, separator)){
				fprintf(stderr, "Failed to configure instance %s\n", current_instance->name);
				goto bail;
			}
		}
	}

	rv = 0;
bail:
	fclose(source);
	free(line_raw);
	return rv;
}
