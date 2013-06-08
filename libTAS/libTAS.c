#include "libTAS.h"

void* SDL_handle;
void(* SDL_GL_SwapBuffers_real)(void);

struct timeval current_time = { 0, 0 };
unsigned long frame_counter = 0;
unsigned int speed_divisor_factor = 1;
unsigned char running = 0;
unsigned char replaying = 0;

unsigned char* recorded_inputs;
unsigned long max_inputs;
int replay_inputs_file;
unsigned long max_inputs_to_replay;

Uint8 key_states[SDLK_LAST] = { 0 };
int socket_fd = 0;

void __attribute__((constructor)) init(void)
{
    // SMB uses its own version of SDL.
    SDL_handle = dlopen("/usr/local/games/supermeatboy/amd64/libSDL-1.2.so.0",
                        RTLD_LAZY);
    if (!SDL_handle)
    {
        log_err("Could not load SDL.");
        exit(-1);
    }

    *(void**)&SDL_GL_SwapBuffers_real = dlsym(SDL_handle, "SDL_GL_SwapBuffers");
    if (!SDL_GL_SwapBuffers_real)
    {
        log_err("Could not import symbol SDL_GL_SwapBuffers.");
        exit(-1);
    }

    if (!unlink(SOCKET_FILENAME))
        log_err("Removed stall socket.");

    const struct sockaddr_un addr = { AF_UNIX, SOCKET_FILENAME };
    const int tmp_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bind(tmp_fd, (const struct sockaddr*)&addr, sizeof(struct sockaddr_un)))
    {
        log_err("Couldn’t bind client socket.");
        exit(-1);
    }

    if (listen(tmp_fd, 1))
    {
        log_err("Couldn’t listen on client socket.");
        exit(1);
    }

    log_err("Loading complete, awaiting client connection...");

    if ((socket_fd = accept(tmp_fd, NULL, NULL)) < 0)
    {
        log_err("Couldn’t accept client connection.");
        exit(1);
    }

    log_err("Client connected.");

    close(tmp_fd);
    unlink(SOCKET_FILENAME);

    recorded_inputs = malloc(sizeof(unsigned char) * 8192);
    max_inputs = 8192;

    proceed_commands();
}

void __attribute__((destructor)) term(void)
{
    dlclose(SDL_handle);
    close(socket_fd);

    log_err("Exiting.");
}

time_t time(time_t* t)
{
    if (t)
        *t = current_time.tv_sec;
    return current_time.tv_sec;
}

int gettimeofday(struct timeval* tv, void* tz)
{
    *tv = current_time;
    return 0;
}

void SDL_GL_SwapBuffers(void)
{
    SDL_GL_SwapBuffers_real();

    /* Once the frame is drawn, we can increment the current time by 1/60 of a
       second. This does not give an integer number of microseconds though, so
       we have to keep track of the fractional part of the microseconds
       counter, and add a spare microsecond each time it's needed. */
    const unsigned int fps = 60;
    const unsigned int integer_increment = 1000000 / fps;
    const unsigned int fractional_increment = 1000000 % fps;
    static unsigned int fractional_part = fps / 2;

    current_time.tv_usec += integer_increment;
    fractional_part += fractional_increment;
    if (fractional_part >= fps)
    {
        ++current_time.tv_usec;
        fractional_part -= fps;
    }

    if (current_time.tv_usec == 1000000)
    {
        ++current_time.tv_sec;
        current_time.tv_usec = 0;
    }

    record_inputs();
    ++frame_counter;

    if (replaying)
    {
        replay_inputs();
        return;
    }

    if (running && speed_divisor_factor > 1)
        usleep(1000000 * (speed_divisor_factor - 1) / 60);

    proceed_commands();
}

Uint8* SDL_GetKeyState(int* keynums)
{
    return key_states;
}

int SDL_PollEvent(void* event)
{
    return 0;
}

void proceed_commands(void)
{
    while (1)
    {
        unsigned int command;

        if (recv(socket_fd, &command, sizeof(unsigned int), MSG_DONTWAIT * running) < 0)
            return;
        else
        {
            char filename_buffer[1024];
            switch (command)
            {
            case 0:
                send(socket_fd, &frame_counter, sizeof(unsigned long), 0);
                break;

            case 1:
                key_states[SDLK_UP] = !key_states[SDLK_UP];
                break;

            case 2:
                key_states[SDLK_DOWN] = !key_states[SDLK_DOWN];
                break;

            case 3:
                key_states[SDLK_LEFT] = !key_states[SDLK_LEFT];
                break;

            case 4:
                key_states[SDLK_RIGHT] = !key_states[SDLK_RIGHT];
                break;

            case 5:
                key_states[SDLK_SPACE] = !key_states[SDLK_SPACE];
                break;

            case 6:
                key_states[SDLK_LSHIFT] = !key_states[SDLK_LSHIFT];
                break;

            case 7:
                running = !running;
                break;

            case 8:
                return;

            case 9:
                recv(socket_fd, &speed_divisor_factor, sizeof(unsigned int), 0);
                break;

            case 10:
                recv(socket_fd, filename_buffer, 1024, 0);
                unsigned long first_frame;
                recv(socket_fd, &first_frame, sizeof(unsigned long), 0);
                int save_inputs_file = open(filename_buffer, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (save_inputs_file < 0)
                {
                    log_err("Couldn’t open inputs file.");
                    break;
                }
                if (first_frame >= frame_counter)
                {
                    log_err("Selected first frame deosn’t exist yet.");
                    close(save_inputs_file);
                    break;
                }
                log_err("Saving inputs...");
                if (write(save_inputs_file, recorded_inputs + first_frame, frame_counter - first_frame) < 0)
                {
                    log_err("Couldn’t save inputs.");
                    close(save_inputs_file);
                    break;
                }
                log_err("Inputs saved.");
                close(save_inputs_file);

                break;

            case 11:
                recv(socket_fd, filename_buffer, 1024, 0);
                replay_inputs_file = open(filename_buffer, O_RDONLY);
                if (replay_inputs_file < 0)
                {
                    log_err("Couldn’t open inputs file.");
                    break;
                }
                log_err("Input file opened, replaying...");

                max_inputs_to_replay = lseek(replay_inputs_file, 0, SEEK_END);
                if (!max_inputs_to_replay)
                {
                    log_err("File is empty, no input to replay.");
                    close(replay_inputs_file);
                    break;
                }
                lseek(replay_inputs_file, 0, SEEK_SET);
                replaying = 1;
                replay_inputs();
                return;

            default:
                log_err("Unknown command recieved.");
            }
        }
    }
}

void record_inputs(void)
{
    if (max_inputs == frame_counter)
    {
        max_inputs *= 2;
        recorded_inputs = realloc(recorded_inputs, sizeof(unsigned char) * max_inputs);
    }

    recorded_inputs[frame_counter] =
        key_states[SDLK_UP] |
        key_states[SDLK_LEFT] << 1 |
        key_states[SDLK_DOWN] << 2 |
        key_states[SDLK_RIGHT] << 3 |
        key_states[SDLK_SPACE] << 4 |
        key_states[SDLK_LSHIFT] << 5;
}

void replay_inputs(void)
{
    unsigned char inputs;

    if (read(replay_inputs_file, &inputs, 1) <= 0)
    {
        log_err("Error reading inputs.");
        exit(-1);
    }

    key_states[SDLK_UP] = inputs & 0x1;
    key_states[SDLK_LEFT] = (inputs >> 1) & 0x1;
    key_states[SDLK_DOWN] = (inputs >> 2) & 0x1;
    key_states[SDLK_RIGHT] = (inputs >> 3) & 0x1;
    key_states[SDLK_SPACE] = (inputs >> 4) & 0x1;
    key_states[SDLK_LSHIFT] = (inputs >> 5) & 0x1;

    if (!--max_inputs_to_replay)
    {
        close(replay_inputs_file);
        replaying = 0;
        log_err("Replaying complete.");
        send(socket_fd, &inputs, 1, 0);
    }
}
