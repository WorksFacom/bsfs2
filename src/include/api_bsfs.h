/*
 * Arquivo de header com as funções de API para utilização
 * nos comandos do browser
 * */

#include "filesystem_bsfs.h"

// Operações com arquivos
// Retorna 0 em caso de sucesso ou -1 em caso de erro
int open_bsfs(FILE *disk, superblock_t *sb, const char *path, file_t *file);

// Retorna 0 em caso de sucesso ou -1 em caso de erro
int close_bsfs(file_t *file);

// Retorna 0 em caso de sucesso ou -1 em caso de erro
int seek_bsfs(file_t *file, uint32_t pos);

// Lê os dados length bytes a partir do valor do campo pos de file
// Armazena em data, retorna o número de bytes lidos
size_t read_bsfs(FILE *disk, superblock_t *sb, file_t *file, void *data,
                 uint32_t length);

// Escreve data de length bytes a partir do valor do campo pos de file
// Retorna o número de bytes escritos
size_t write_bsfs(FILE *disk, superblock_t *sb, file_t *file, void *data,
                  uint32_t length);

// Cria um novo arquivo no caminho especificado.
// Retorna 0 em sucesso, -1 em erro (ex: já existe, sem espaço, etc)
int create_file_bsfs(FILE *disk, superblock_t *sb, const char *path);

// Remove um arquivo dado seu caminho
// Retorna 0 em sucesso, -1 em erro
int remove_file_bsfs(FILE *disk, superblock_t *sb, const char *path);

// Move ou renomeia um arquivo (ou diretório) de um caminho para outro
// Retorna 0 em sucesso, -1 em erro
int move_bsfs(FILE *disk, superblock_t *sb, const char *old_path,
              const char *new_path);

// Retorna informações de inode associado ao caminho
int stat_bsfs(FILE *disk, superblock_t *sb, const char *path, inode_t *inode);

// Abre diretório especificado por inode
// Retorna 0 em sucesso, -1 em erro
int opendir_bsfs(FILE *disk, superblock_t *sb, const char *dirname, dir_t *dir);

// Fecha diretório
int closedir_bsfs(dir_t *dir);

// Lê próxima entrada do diretório
// Retorna 1 se sucesso, 0 se fim do diretório, -1 em erro
int readdir_bsfs(FILE *disk, superblock_t *sb, dir_t *dir, dir_entry_t *entry);

// Cria novo diretório no caminho especificado
// Retorna 0 se sucesso, -1 se erro
int mkdir_bsfs(FILE *disk, superblock_t *sb, const char *path);

// Remove diretório (deve estar vazio)
// Retorna 0 se sucesso, -1 se erro
int rmdir_bsfs(FILE *disk, superblock_t *sb, const char *path);

// Muda diretório atual
// Retorna 0 se sucesso, -1 se erro
int chdir_bsfs(FILE *disk, superblock_t *sb, const char *path);

// Obtém caminho do diretório atual
// Retorna 0 se sucesso, -1 se erro
int getcwd_bsfs(FILE *disk, superblock_t *sb, char *path_buffer, size_t buffer_size);