"""
Hybrid min-heap + red-black tree allocator.

Recycled indices are kept in both a min-heap (cheap minimum) and an RB tree. `position_map` tracks
heap positions so an arbitrary key can be removed. Allocation takes the smaller of heap top and
`next_fresh`, removing the key from both structures or advancing the cursor. Frees insert into
both and never move the cursor. The RB tree has no say in which block gets picked.
"""

from core import BaseAllocator, register


class Color:
    RED = 0
    BLACK = 1


class RBNode:
    def __init__(self, key):
        self.key = key
        self.color = Color.RED
        self.left = None
        self.right = None
        self.parent = None


class HybridRBTreeForDealloc:
    def __init__(self):
        self.root = None

    def insert(self, key):
        new_node = RBNode(key)

        if not self.root:
            new_node.color = Color.BLACK
            self.root = new_node
            return

        parent = None
        current = self.root

        while current:
            parent = current
            if key < current.key:
                current = current.left
            elif key > current.key:
                current = current.right
            else:
                return

        new_node.parent = parent
        if key < parent.key:
            parent.left = new_node
        else:
            parent.right = new_node

        self._fix_insert(new_node)

    def delete(self, key):
        node = self._find(self.root, key)
        if not node:
            return

        if node.left and node.right:
            successor = self._min_node(node.right)
            node.key = successor.key
            node = successor

        child = node.left if node.left else node.right

        if child:
            child.parent = node.parent

        if not node.parent:
            self.root = child
        else:
            if node.parent.left == node:
                node.parent.left = child
            else:
                node.parent.right = child

        if node.color == Color.BLACK:
            if child:
                self._fix_delete(child)
            elif node.parent:
                self._fix_delete_null(node.parent, node == node.parent.left)

    def _find(self, node, key):
        while node:
            if key == node.key:
                return node
            elif key < node.key:
                node = node.left
            else:
                node = node.right
        return None

    def _min_node(self, node):
        while node.left:
            node = node.left
        return node

    def _fix_insert(self, node):
        while node.parent and node.parent.color == Color.RED:
            if not node.parent.parent:
                break
            if node.parent == node.parent.parent.left:
                uncle = node.parent.parent.right

                if uncle and uncle.color == Color.RED:
                    node.parent.color = Color.BLACK
                    uncle.color = Color.BLACK
                    node.parent.parent.color = Color.RED
                    node = node.parent.parent
                else:
                    if node == node.parent.right:
                        node = node.parent
                        self._rotate_left(node)

                    node.parent.color = Color.BLACK
                    node.parent.parent.color = Color.RED
                    self._rotate_right(node.parent.parent)
            else:
                uncle = node.parent.parent.left

                if uncle and uncle.color == Color.RED:
                    node.parent.color = Color.BLACK
                    uncle.color = Color.BLACK
                    node.parent.parent.color = Color.RED
                    node = node.parent.parent
                else:
                    if node == node.parent.left:
                        node = node.parent
                        self._rotate_right(node)

                    node.parent.color = Color.BLACK
                    node.parent.parent.color = Color.RED
                    self._rotate_left(node.parent.parent)

        self.root.color = Color.BLACK

    def _fix_delete(self, node):
        while node != self.root and self._get_color(node) == Color.BLACK:
            if node == node.parent.left:
                sibling = node.parent.right

                if self._get_color(sibling) == Color.RED:
                    sibling.color = Color.BLACK
                    node.parent.color = Color.RED
                    self._rotate_left(node.parent)
                    sibling = node.parent.right

                left_color = self._get_color(sibling.left) if sibling else Color.BLACK
                right_color = self._get_color(sibling.right) if sibling else Color.BLACK

                if left_color == Color.BLACK and right_color == Color.BLACK:
                    if sibling:
                        sibling.color = Color.RED
                    node = node.parent
                else:
                    if right_color == Color.BLACK:
                        if sibling.left:
                            sibling.left.color = Color.BLACK
                        sibling.color = Color.RED
                        self._rotate_right(sibling)
                        sibling = node.parent.right

                    if sibling:
                        sibling.color = node.parent.color
                    node.parent.color = Color.BLACK
                    if sibling and sibling.right:
                        sibling.right.color = Color.BLACK
                    self._rotate_left(node.parent)
                    node = self.root
            else:
                sibling = node.parent.left

                if self._get_color(sibling) == Color.RED:
                    sibling.color = Color.BLACK
                    node.parent.color = Color.RED
                    self._rotate_right(node.parent)
                    sibling = node.parent.left

                left_color = self._get_color(sibling.left) if sibling else Color.BLACK
                right_color = self._get_color(sibling.right) if sibling else Color.BLACK

                if right_color == Color.BLACK and left_color == Color.BLACK:
                    if sibling:
                        sibling.color = Color.RED
                    node = node.parent
                else:
                    if left_color == Color.BLACK:
                        if sibling.right:
                            sibling.right.color = Color.BLACK
                        sibling.color = Color.RED
                        self._rotate_left(sibling)
                        sibling = node.parent.left

                    if sibling:
                        sibling.color = node.parent.color
                    node.parent.color = Color.BLACK
                    if sibling and sibling.left:
                        sibling.left.color = Color.BLACK
                    self._rotate_right(node.parent)
                    node = self.root

    def _fix_delete_null(self, parent, is_left):
        node = None
        while parent and (node is None or node.color == Color.BLACK):
            if not node or node == parent.left:
                sibling = parent.right

                if sibling and sibling.color == Color.RED:
                    sibling.color = Color.BLACK
                    parent.color = Color.RED
                    self._rotate_left(parent)
                    sibling = parent.right

                if sibling:
                    left_color = self._get_color(sibling.left)
                    right_color = self._get_color(sibling.right)

                    if left_color == Color.BLACK and right_color == Color.BLACK:
                        sibling.color = Color.RED
                        node = parent
                        parent = node.parent
                    else:
                        if right_color == Color.BLACK:
                            if sibling.left:
                                sibling.left.color = Color.BLACK
                            sibling.color = Color.RED
                            self._rotate_right(sibling)
                            sibling = parent.right

                        sibling.color = parent.color
                        parent.color = Color.BLACK
                        if sibling and sibling.right:
                            sibling.right.color = Color.BLACK
                        self._rotate_left(parent)
                        break
                else:
                    node = parent
                    parent = node.parent
            else:
                sibling = parent.left

                if sibling and sibling.color == Color.RED:
                    sibling.color = Color.BLACK
                    parent.color = Color.RED
                    self._rotate_right(parent)
                    sibling = parent.left

                if sibling:
                    left_color = self._get_color(sibling.left)
                    right_color = self._get_color(sibling.right)

                    if right_color == Color.BLACK and left_color == Color.BLACK:
                        sibling.color = Color.RED
                        node = parent
                        parent = node.parent
                    else:
                        if left_color == Color.BLACK:
                            if sibling.right:
                                sibling.right.color = Color.BLACK
                            sibling.color = Color.RED
                            self._rotate_left(sibling)
                            sibling = parent.left

                        sibling.color = parent.color
                        parent.color = Color.BLACK
                        if sibling and sibling.left:
                            sibling.left.color = Color.BLACK
                        self._rotate_right(parent)
                        break
                else:
                    node = parent
                    parent = node.parent

    def _get_color(self, node):
        return node.color if node else Color.BLACK

    def _rotate_left(self, node):
        right_child = node.right
        node.right = right_child.left

        if right_child.left:
            right_child.left.parent = node

        right_child.parent = node.parent

        if not node.parent:
            self.root = right_child
        elif node.parent.left == node:
            node.parent.left = right_child
        else:
            node.parent.right = right_child

        right_child.left = node
        node.parent = right_child

    def _rotate_right(self, node):
        left_child = node.left
        node.left = left_child.right

        if left_child.right:
            left_child.right.parent = node

        left_child.parent = node.parent

        if not node.parent:
            self.root = left_child
        elif node.parent.left == node:
            node.parent.left = left_child
        else:
            node.parent.right = left_child

        left_child.right = node
        node.parent = left_child

    def contains(self, key):
        return self._find(self.root, key) is not None


@register("hybrid", display_name="Hybrid Heap+RB")
class BitmapHybridHeapRB(BaseAllocator):
    def __init__(self, size, heap_order=256):
        super().__init__(size)
        self.heap_order = heap_order
        self.heap = []
        self.position_map = {}
        self.next_fresh = 0

        # RB tree kept for API compatibility, nothing is preloaded
        self.rbtree = HybridRBTreeForDealloc()

    def find_free_block(self):
        """Smallest free index across the heap and the fresh cursor."""
        recycled_min = self.heap[0] if self.heap else -1
        fresh_min = self.next_fresh if self.next_fresh < self.size else -1
        if recycled_min == -1:
            return fresh_min
        if fresh_min == -1:
            return recycled_min
        return min(recycled_min, fresh_min)

    def allocate_blocks(self, count):
        if count <= 0:
            return []
        if self.free_blocks_remaining < count:
            return []

        allocated = []

        for _ in range(count):
            idx = self.find_free_block()
            if idx == -1:
                return []

            if self.heap and idx == self.heap[0]:
                self._delete_key(idx)
            elif idx == self.next_fresh:
                self.next_fresh += 1
            self.bitmap[idx] = 1
            allocated.append(idx)

        return allocated

    def deallocate_blocks(self, indices):
        if not self.validate_deallocation(indices):
            return False

        for idx in indices:
            self.bitmap[idx] = 0
            self._insert_key(idx)
        return True

    def count_free_blocks(self):
        return self.free_blocks_remaining

    # ===== internals =====

    def _insert_key(self, key):
        self.rbtree.insert(key)

        if key not in self.position_map:
            pos = len(self.heap)
            self.heap.append(key)
            self.position_map[key] = pos
            self._sift_up(pos)

    def _delete_key(self, key):
        self.rbtree.delete(key)

        if key in self.position_map:
            pos = self.position_map[key]
            last_key = self.heap[-1]

            if key != last_key:
                self.heap[pos] = last_key
                self.position_map[last_key] = pos

                if pos > 0 and self.heap[pos] < self.heap[(pos - 1) // 2]:
                    self._sift_up(pos)
                else:
                    self._sift_down(pos)

            self.heap.pop()
            del self.position_map[key]

    def _sift_up(self, pos):
        key = self.heap[pos]
        while pos > 0:
            parent_pos = (pos - 1) // 2
            parent_key = self.heap[parent_pos]

            if key < parent_key:
                self.heap[pos] = parent_key
                self.position_map[parent_key] = pos
                pos = parent_pos
            else:
                break

        self.heap[pos] = key
        self.position_map[key] = pos

    def _sift_down(self, pos):
        key = self.heap[pos]
        size = len(self.heap)

        while True:
            smallest = pos
            left = 2 * pos + 1
            right = 2 * pos + 2

            if left < size and self.heap[left] < self.heap[smallest]:
                smallest = left

            if right < size and self.heap[right] < self.heap[smallest]:
                smallest = right

            if smallest != pos:
                self.heap[pos] = self.heap[smallest]
                self.position_map[self.heap[pos]] = pos
                pos = smallest
            else:
                break

        self.heap[pos] = key
        self.position_map[key] = pos
