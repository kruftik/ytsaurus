package ru.yandex.spark.yt.fs

import java.io.FileNotFoundException

import org.apache.hadoop.fs._
import org.apache.hadoop.fs.permission.FsPermission
import org.apache.hadoop.util.Progressable
import org.apache.log4j.Logger
import ru.yandex.spark.yt.wrapper.YtWrapper
import ru.yandex.spark.yt.wrapper.cypress.PathType
import ru.yandex.yt.ytclient.proxy.YtClient

import scala.language.postfixOps

@SerialVersionUID(1L)
class YtTableFileSystem extends YtFileSystemBase {
  private val log = Logger.getLogger(getClass)

  override def listStatus(f: Path): Array[FileStatus] = {
    log.debugLazy(s"List status $f")
    implicit val ytClient: YtClient = yt
    val path = ytPath(f)

    val transaction = GlobalTableSettings.getTransaction(path)
    val pathType = YtWrapper.pathType(path, transaction)

    pathType match {
      case PathType.File => Array(getFileStatus(f))
      case PathType.Table => listTableAsFiles(f, path, transaction)
      case PathType.Directory => listYtDirectory(f, path, transaction)
      case _ => throw new IllegalArgumentException(s"Can't list $pathType")
    }
  }

  private def listTableAsFiles(f: Path, path: String, transaction: Option[String])
                              (implicit yt: YtClient): Array[FileStatus] = {
    val rowCount = YtWrapper.attribute(path, "row_count", transaction).longValue()
    val chunksCount = GlobalTableSettings.getFilesCount(path).getOrElse(
      YtWrapper.attribute(path, "chunk_count", transaction).longValue().toInt
    )
    GlobalTableSettings.removeFilesCount(path)
    val filesCount = if (chunksCount > 0) chunksCount else 1
    val result = new Array[FileStatus](filesCount)
    for (chunkIndex <- 0 until chunksCount) {
      val chunkStart = chunkIndex * rowCount / chunksCount
      val chunkRowCount = (chunkIndex + 1) * rowCount / chunksCount - chunkStart
      val chunkPath = new YtPath(f, chunkStart, chunkRowCount)
      result(chunkIndex) = new YtFileStatus(chunkPath, rowCount, chunksCount)
    }
    if (chunksCount == 0) {
      // add path for schema resolving
      val chunkPath = new YtPath(f, 0, 0)
      result(0) = new YtFileStatus(chunkPath, rowCount, chunksCount)
    }
    result
  }

  override def getFileStatus(f: Path): FileStatus = {
    log.debugLazy(s"Get file status $f")
    implicit val ytClient: YtClient = yt
    val path = ytPath(f)
    val transaction = GlobalTableSettings.getTransaction(path)

    f match {
      case yp: YtPath =>
        new FileStatus(yp.rowCount, false, 1, yp.rowCount, 0, yp)
      case _ =>
        if (!YtWrapper.exists(path, transaction)) {
          throw new FileNotFoundException(s"File $path is not found")
        } else {
          val pathType = YtWrapper.pathType(path, transaction)
          pathType match {
            case PathType.Table => new FileStatus(0, true, 1, 0, 0, f)
            case PathType.File => new FileStatus(YtWrapper.fileSize(path, transaction), false, 1, 0, 0, f)
            case PathType.Directory => new FileStatus(0, true, 1, 0, 0, f)
            case PathType.None => null
          }
        }
    }
  }

  override def create(f: Path, permission: FsPermission, overwrite: Boolean, bufferSize: Int,
                      replication: Short, blockSize: Long, progress: Progressable): FSDataOutputStream = {
    create(f, permission, overwrite, bufferSize, replication, blockSize, progress, statistics)
  }
}
