#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_KEYS 3 // Maximum keys per node
#define MIN_KEYS 1 // Minimum keys per node (except root)
//
// 15, 30, 45, 60, 75, 10, 25, 40, 55, 70, 5, 20, 35, 50, 65, 80, 12, 28, 42, 58
// t = 2
// t - 1 = 1
// 2t - 1 = 3

typedef struct bpnode {
  int keys[MAX_KEYS + 1];
  struct bpnode *children[MAX_KEYS + 2];
  int num_keys;
  int is_leaf;
} bpnode;

typedef struct bptree {
  bpnode *root;
} bptree;

bpnode *create_node(int is_leaf) {
  bpnode *node = (bpnode *)calloc(sizeof(bpnode), 1);
  node->is_leaf = is_leaf;
  return node;
}

bptree *bptree_init() {
  bptree *tree = (bptree *)malloc(sizeof(bptree));
  tree->root = create_node(true);
  return tree;
}

// bottom-up, needs to be converted to top-bottom
int insert_in_leaf(bpnode *leaf, int key) {
  // Check for duplicates
  for (int i = 0; i < leaf->num_keys; i++) {
    if (leaf->keys[i] == key) {
      return 0; // Duplicate, don't insert
    }
  }

  int i = leaf->num_keys - 1;
  while (i >= 0 && key < leaf->keys[i] && i < leaf->num_keys) {
    leaf->keys[i + 1] = leaf->keys[i];
    i--;
  }

  leaf->keys[i + 1] = key;
  leaf->num_keys++;

  int need_split = leaf->num_keys > MAX_KEYS;
  return need_split;
}

bpnode *split_leaf_node(bpnode *leaf) {
  bpnode *new_leaf = create_node(true);

  int mid = (MAX_KEYS + 1) / 2;

  // Copy second half to new leaf
  new_leaf->num_keys = leaf->num_keys - mid;
  for (int i = 0; i < new_leaf->num_keys; i++) {
    new_leaf->keys[i] = leaf->keys[mid + i];
  }

  leaf->num_keys = mid;

  return new_leaf;
}

bpnode *split_internal_node(bpnode *node, int *split_key) {
  bpnode *new_node = create_node(false);

  int mid = (MAX_KEYS + 1) / 2;

  new_node->num_keys = node->num_keys - mid - 1;
  for (int i = 1; i <= new_node->num_keys; i++) {
    new_node->keys[i - 1] = node->keys[mid + i];
    new_node->children[i - 1] = node->children[mid + i];
  }
  new_node->children[new_node->num_keys] = node->children[node->num_keys];

  node->num_keys = mid;

  *split_key = node->keys[mid];
  return new_node;
}

void insert_in_parent(bptree *tree, bpnode *old_node, int key, bpnode *new_node, bpnode **path, int path_len) {
  if (path_len == 0) {
    // old_node was root, create new root
    bpnode *new_root = create_node(false);
    new_root->keys[0] = key;
    new_root->children[0] = old_node;
    new_root->children[1] = new_node;

    new_root->num_keys = 1;
    tree->root = new_root;
    return;
  }

  bpnode *parent = path[path_len - 1];

  int i = parent->num_keys - 1;
  while (i >= 0 && key < parent->keys[i]) {
    parent->keys[i + 1] = parent->keys[i];
    parent->children[i + 2] = parent->children[i + 1];
    i--;
  }

  parent->keys[i + 1] = key;
  parent->children[i + 2] = new_node;
  parent->num_keys++;

  // Check if parent needs to split
  if (parent->num_keys > MAX_KEYS) {
    int split_key;
    bpnode *new_parent = split_internal_node(parent, &split_key);
    insert_in_parent(tree, parent, split_key, new_parent, path, path_len - 1);
  }
}

void bptree_insert(bptree *tree, int key) {
  bpnode *path[100];
  int path_len = 0;

  bpnode *current = tree->root;

  while (!current->is_leaf) {
    path[path_len++] = current;

    int i = 0;
    while (i < current->num_keys && key >= current->keys[i]) {
      i++;
    }
    current = current->children[i];
  }

  // current is now the leaf
  int need_split = insert_in_leaf(current, key);

  if (need_split) {
    bpnode *new_leaf = split_leaf_node(current);
    int split_key = new_leaf->keys[0]; // First key of new leaf goes up
    insert_in_parent(tree, current, split_key, new_leaf, path, path_len);
  }
}

int remove_from_leaf(bpnode *leaf, int key, int *is_leftmost) {
  int i = 0;
  while (i < leaf->num_keys && leaf->keys[i] != key) {
    i++;
  }

  if (i == leaf->num_keys) {
    return 0; // error
  }

  *is_leftmost = i == 0 ? 1 : 0;

  // Shift keys left
  for (int j = i; j < leaf->num_keys - 1; j++) {
    leaf->keys[j] = leaf->keys[j + 1];
  }
  leaf->num_keys--;

  return 1;
}

int borrow_from_left_leaf(bpnode *leaf, bpnode *left_sibling, bpnode *parent, int parent_key_idx) {
  if (left_sibling->num_keys <= MIN_KEYS) {
    return false;
  }

  // Shift all keys in leaf right
  for (int i = leaf->num_keys; i > 0; i--) {
    leaf->keys[i] = leaf->keys[i - 1];
  }

  // Move last key from left sibling to leaf
  leaf->keys[0] = left_sibling->keys[left_sibling->num_keys - 1];
  leaf->num_keys++;
  left_sibling->num_keys--;

  parent->keys[parent_key_idx] = leaf->keys[0];

  return true;
}

int borrow_from_right_leaf(bpnode *leaf, bpnode *right_sibling, bpnode *parent, int parent_key_idx) {
  if (right_sibling->num_keys <= MIN_KEYS) {
    return false;
  }

  // Move first key from right sibling to leaf
  leaf->keys[leaf->num_keys] = right_sibling->keys[0];
  leaf->num_keys++;

  // Shift keys in right sibling left
  for (int i = 0; i < right_sibling->num_keys - 1; i++) {
    right_sibling->keys[i] = right_sibling->keys[i + 1];
  }
  right_sibling->num_keys--;

  parent->keys[parent_key_idx] = right_sibling->keys[0];

  return true;
}

void merge_with_left_leaf(bpnode *leaf, bpnode *left_sibling, bpnode *parent, int parent_key_idx) {
  // Copy all keys from leaf to left sibling
  for (int i = 0; i < leaf->num_keys; i++) {
    left_sibling->keys[left_sibling->num_keys + i] = leaf->keys[i];
  }
  left_sibling->num_keys += leaf->num_keys;

  // Remove key from parent
  for (int i = parent_key_idx; i < parent->num_keys - 1; i++) {
    parent->keys[i] = parent->keys[i + 1];
  }

  // Remove pointer from parent
  for (int i = parent_key_idx + 1; i <= parent->num_keys - 1; i++) {
    parent->children[i] = parent->children[i + 1];
  }
  parent->num_keys--;

  free(leaf);
}

void merge_with_right_leaf(bpnode *leaf, bpnode *right_sibling, bpnode *parent, int parent_key_idx) {
  // Copy all keys from right sibling to leaf
  for (int i = 0; i < right_sibling->num_keys; i++) {
    leaf->keys[leaf->num_keys + i] = right_sibling->keys[i];
  }
  leaf->num_keys += right_sibling->num_keys;

  // Remove key from parent
  for (int i = parent_key_idx; i < parent->num_keys - 1; i++) {
    parent->keys[i] = parent->keys[i + 1];
  }

  // Remove pointer from parent
  for (int i = parent_key_idx + 1; i <= parent->num_keys - 1; i++) {
    parent->children[i] = parent->children[i + 1];
  }
  parent->num_keys--;

  free(right_sibling);
}

int borrow_from_left_internal(bpnode *node, bpnode *left_sibling, bpnode *parent, int parent_key_idx) {
  if (left_sibling->num_keys <= MIN_KEYS) {
    return false;
  }

  // Shift all keys and children in node right
  for (int i = node->num_keys; i > 0; i--) {
    node->keys[i] = node->keys[i - 1];
  }
  for (int i = node->num_keys + 1; i > 0; i--) {
    node->children[i] = node->children[i - 1];
  }

  // Move parent key down to node
  node->keys[0] = parent->keys[parent_key_idx - 1];
  node->children[0] = left_sibling->children[left_sibling->num_keys];
  node->num_keys++;

  // Move key from left sibling up to parent
  parent->keys[parent_key_idx - 1] = left_sibling->keys[left_sibling->num_keys - 1];
  left_sibling->num_keys--;

  return true;
}

int borrow_from_right_internal(bpnode *node, bpnode *right_sibling, bpnode *parent, int parent_key_idx) {
  if (right_sibling->num_keys <= MIN_KEYS) {
    return false;
  }

  // Move parent key down to node
  node->keys[node->num_keys] = parent->keys[parent_key_idx];
  node->children[node->num_keys + 1] = right_sibling->children[0];
  node->num_keys++;

  // Move key from right sibling up to parent
  parent->keys[parent_key_idx] = right_sibling->keys[0];

  // Shift keys and children in right sibling left
  for (int i = 0; i < right_sibling->num_keys - 1; i++) {
    right_sibling->keys[i] = right_sibling->keys[i + 1];
  }
  for (int i = 0; i < right_sibling->num_keys; i++) {
    right_sibling->children[i] = right_sibling->children[i + 1];
  }
  right_sibling->num_keys--;

  return true;
}

void merge_with_left_internal(bpnode *node, bpnode *left_sibling, bpnode *parent, int parent_key_idx) {
  // Move parent key down to left sibling
  left_sibling->keys[left_sibling->num_keys] = parent->keys[parent_key_idx - 1];
  left_sibling->num_keys++;

  // Copy all keys and children from node to left sibling
  for (int i = 0; i < node->num_keys; i++) {
    left_sibling->keys[left_sibling->num_keys + i] = node->keys[i];
    left_sibling->children[left_sibling->num_keys + i] = node->children[i];
  }
  left_sibling->children[left_sibling->num_keys + node->num_keys] = node->children[node->num_keys];
  left_sibling->num_keys += node->num_keys;

  // Remove key from parent
  for (int i = parent_key_idx - 1; i < parent->num_keys - 1; i++) {
    parent->keys[i] = parent->keys[i + 1];
  }

  // Remove pointer from parent
  for (int i = parent_key_idx; i <= parent->num_keys - 1; i++) {
    parent->children[i] = parent->children[i + 1];
  }
  parent->num_keys--;

  free(node);
}

void merge_with_right_internal(bpnode *node, bpnode *right_sibling, bpnode *parent, int parent_key_idx) {
  // Move parent key down to node
  node->keys[node->num_keys] = parent->keys[parent_key_idx];
  node->num_keys++;

  // Copy all keys and children from right sibling to node
  for (int i = 0; i < right_sibling->num_keys; i++) {
    node->keys[node->num_keys + i] = right_sibling->keys[i];
    node->children[node->num_keys + i] = right_sibling->children[i];
  }
  node->children[node->num_keys + right_sibling->num_keys] = right_sibling->children[right_sibling->num_keys];
  node->num_keys += right_sibling->num_keys;

  // Remove key from parent
  for (int i = parent_key_idx; i < parent->num_keys - 1; i++) {
    parent->keys[i] = parent->keys[i + 1];
  }

  // Remove pointer from parent
  for (int i = parent_key_idx + 1; i <= parent->num_keys - 1; i++) {
    parent->children[i] = parent->children[i + 1];
  }
  parent->num_keys--;

  free(right_sibling);
}

void fix_underflow(bptree *tree, bpnode *node, bpnode **path, int path_len) {
  // Root is special case
  if (path_len == 0) {
    if (node->num_keys == 0 && !node->is_leaf) {
      tree->root = node->children[0];
      free(node);
    }
    return;
  }

  bpnode *parent = path[path_len - 1];

  int child_idx = -1;
  for (int i = 0; i <= parent->num_keys; i++) {
    if (parent->children[i] == node) {
      child_idx = i;
    }
  }

  if (child_idx == -1) {
    printf("Error: child not found.\n");
    return; // Error: child not found
  }

  bpnode *left_sibling = (child_idx > 0) ? parent->children[child_idx - 1] : NULL;
  bpnode *right_sibling = (child_idx < parent->num_keys) ? parent->children[child_idx + 1] : NULL;

  if (node->is_leaf) {
    // Try borrowing from siblings
    if (left_sibling && borrow_from_left_leaf(node, left_sibling, parent, child_idx - 1)) {
      return;
    }
    if (right_sibling && borrow_from_right_leaf(node, right_sibling, parent, child_idx)) {
      return;
    }

    // Merge with a sibling
    if (left_sibling) {
      merge_with_left_leaf(node, left_sibling, parent, child_idx);
    } else if (right_sibling) {
      merge_with_right_leaf(node, right_sibling, parent, child_idx + 1);
    }
  } else {
    // Internal node
    if (left_sibling && borrow_from_left_internal(node, left_sibling, parent, child_idx)) {
      return;
    }
    if (right_sibling && borrow_from_right_internal(node, right_sibling, parent, child_idx)) {
      return;
    }

    // Merge with a sibling
    if (left_sibling) {
      merge_with_left_internal(node, left_sibling, parent, child_idx);
    } else if (right_sibling) {
      merge_with_right_internal(node, right_sibling, parent, child_idx);
    }
  }

  // Check if parent now has underflow
  if (parent->num_keys < MIN_KEYS) {
    fix_underflow(tree, parent, path, path_len - 1);
  }
}

void bptree_remove(bptree *tree, int key) {
  if (tree->root->num_keys == 0) {
    return;
  }

  bpnode *path[100];
  int path_len = 0;

  bpnode *current = tree->root;

  while (!current->is_leaf) {
    path[path_len++] = current;

    int i = 0;
    while (i < current->num_keys && key >= current->keys[i]) {
      i++;
    }
    current = current->children[i];
  }

  int is_leftmost;
  if (!remove_from_leaf(current, key, &is_leftmost)) {
    return; // Key not found
  }

  if (is_leftmost) {
    int new_leftmost = current->keys[0];

    bpnode *node = NULL;
    int index = -1;
    int i = path_len - 1;
    while (i >= 0 && index == -1) {
      node = path[i--];

      for (int j = 0; j < node->num_keys; j++) {
        if (key == node->keys[j]) {
          index = j;
          break;
        }
      }
    }

    if (index != -1) {
      node->keys[index] = new_leftmost;
    }
  }

  if (path_len == 0 && tree->root->num_keys == 0 && !tree->root->is_leaf) {
    bpnode *old_root = tree->root;
    tree->root = tree->root->children[0];
    free(old_root);
    return;
  }

  if (current->num_keys < MIN_KEYS) {
    fix_underflow(tree, current, path, path_len);
  }
}

bpnode *bptree_search(bpnode *node, int key) {
  if (node->is_leaf) {
    for (int i = 0; i < node->num_keys; i++) {
      if (node->keys[i] == key) {
        return node;
      }
    }
    return NULL;
  }

  int i = 0;
  while (i < node->num_keys && key >= node->keys[i]) {
    i++;
  }

  return bptree_search(node->children[i], key);
}

void print_tree(bpnode *node, int level, char *prefix, int is_last) {
  if (node == NULL)
    return;

  // Print current node
  printf("%s", prefix);

  if (level > 0) {
    printf("%s", is_last ? "└── " : "├── ");
  }

  // Print keys in the node
  printf("[");
  for (int i = 0; i < node->num_keys; i++) {
    printf("%d", node->keys[i]);
    if (i < node->num_keys - 1) {
      printf(" ");
    }
  }
  printf("]");

  if (node->is_leaf) {
    printf(" (leaf)");
  } else {
    printf(" (internal)");
  }
  printf("\n");

  // Print children
  if (!node->is_leaf) {
    char new_prefix[1024];
    for (int i = 0; i <= node->num_keys; i++) {
      if (node->children[i] != NULL) {
        strcpy(new_prefix, prefix);
        if (level > 0) {
          strcat(new_prefix, is_last ? "    " : "│   ");
        }
        print_tree(node->children[i], level + 1, new_prefix, i == node->num_keys);
      }
    }
  }
}

void bptree_print_tree(bptree *tree) {
  if (tree->root->num_keys == 0) {
    printf("Empty tree\n");
    return;
  }
  print_tree(tree->root, 0, "", true);
  printf("\n");
}

// Free the tree
void bptree_destroy(bpnode *node) {
  if (node == NULL)
    return;

  if (!node->is_leaf) {
    for (int i = 0; i <= node->num_keys; i++) {
      bptree_destroy(node->children[i]);
    }
  }
  free(node);
}

void print_help() {
  printf("\n\nInteractive Mode\n");
  printf("================\n");
  printf("Commands:\n");
  printf("  i <number> - Insert a number\n");
  printf("  d <number> - Delete a number\n");
  printf("  s <number> - Search for a number\n");
  printf("  p          - Print tree\n");
  printf("  q          - Quit\n\n");
}

int main() {
  print_help();

  bptree *tree = bptree_init();

  // Demo: Insert some values
  // int values[] = {10, 20, 5, 6, 12, 30, 7, 17, 3, 50, 40, 25, 35, 8, 9};
  int values[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  int n = sizeof(values) / sizeof(values[0]);
  for (int i = 0; i < n; i++) {
    bptree_insert(tree, values[i]);
  }

  char command[100];
  while (1) {
    printf("> ");
    if (fgets(command, sizeof(command), stdin) == NULL)
      break;

    if (command[0] == 'q') {
      break;
    } else if (command[0] == 'p') {
      bptree_print_tree(tree);
    } else if (command[0] == 'i') {
      int key;
      if (sscanf(command + 2, "%d", &key) == 1) {
        bptree_insert(tree, key);
        printf("Inserted %d\n", key);
        bptree_print_tree(tree);
      } else {
        printf("Invalid number\n");
      }
    } else if (command[0] == 'd') {
      int key;
      if (sscanf(command + 2, "%d", &key) == 1) {
        bptree_remove(tree, key);
        printf("Deleted %d\n", key);
        bptree_print_tree(tree);
      } else {
        printf("Invalid number\n");
      }
    } else if (command[0] == 's') {
      int key;
      if (sscanf(command + 2, "%d", &key) == 1) {
        bpnode *result = bptree_search(tree->root, key);
        if (result != NULL) {
          printf("Key %d found in leaf!\n", key);
        } else {
          printf("Key %d not found\n", key);
        }
      } else {
        printf("Invalid number\n");
      }
    } else {
      printf("Unknown command\n");
    }
  }

  bptree_destroy(tree->root);
  free(tree);

  return 0;
}
